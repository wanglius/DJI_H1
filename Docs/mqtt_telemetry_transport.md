# MQTT telemetry transport format v02

The B board publishes GPS and calculated reflectance records through one MQTT
uplink topic. The original measurement record remains the logical payload; this
transport layer only divides large byte strings into DTU-safe fragments and
reassembles them. It never changes timestamps or measurement fields.

## Design boundaries

- One encoded fragment is at most 1024 bytes, matching the DTU `UARTTL` limit.
- Every message carries the ESP32's nonzero 48-bit factory MAC identity in a
  64-bit field. A fresh nonzero 64-bit random transport mission ID is generated for
  each flight and recorded in `MISSION.JSON`; neither device clones nor a
  reformatted/replaced card can reuse the complete message identity.
- The ESP32 implementation performs no per-message allocation. A configurable
  shared GPS/reflectance pool (512 entries in the production board profile) is
  allocated once in PSRAM. Only pointers cross the FreeRTOS ready/free queues.
  A slot remains owned until a matching application disposition, the 10-second
  production residency deadline, a permanent local failure, or a shutdown
  abort releases it.
- The caller of the MQTT publish API offers a complete logical message;
  acquisition and calculation tasks must never wait for UART or cellular I/O.
- Integers are serialized explicitly in little-endian order. Compiler struct
  layout is never written to the wire.
- Fragment indices are zero-based.
- A message is identified by
  `(source_id, mission_id, message_type, message_sequence)`.
- Give every DTU its own uplink topic. Because a DTU may divide one DTF2 frame
  across MQTT publications, byte streams from multiple publishers must not be
  interleaved on one topic before framing is recovered.
- Duplicates are expected from application retries and must be ignored by the
  receiver, including duplicates arriving shortly after completion.

## Fragment wire format

Every fragment begins with a 44-byte header and ends with an IEEE CRC-32 over
the header and fragment payload.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `DTF2` |
| 4 | 1 | Fragment protocol version (`2`) |
| 5 | 1 | Message type |
| 6 | 2 | Header size (`44`) |
| 8 | 8 | Source/device identity (factory MAC stored as uint64) |
| 16 | 8 | Random transport mission ID |
| 24 | 4 | Logical-message sequence |
| 28 | 4 | Complete logical-message length |
| 32 | 4 | Complete logical-message CRC-32 |
| 36 | 2 | Fragment index |
| 38 | 2 | Fragment count |
| 40 | 2 | Fragment payload length |
| 42 | 2 | Flags, reserved as zero in v02 |
| 44 | 0..976 | Fragment payload |
| variable | 4 | Fragment CRC-32 |

The 976-byte maximum follows from `1024 - 44 - 4`. A 98-byte GPS record uses
one 146-byte fragment. The current 711-sample reflectance record is 2233 bytes
and uses three fragments with wire sizes `1024, 1024, 329`. The schema maximum
of 1024 samples is 3172 bytes and uses four fragments with wire sizes
`1024, 1024, 1024, 292`.

Message-type values intentionally match the existing measurement record types:
GPS `1`, raw spectrum `2`, reflectance `3`, and operation log `4`. The initial
production publisher will send GPS and reflectance only.

## Production sender

The `telemetry` component owns UART1 on GPIO17/GPIO18 and is the only task that
writes application data to the DTU. The measurement recorder gives it finalized
v01 GPS and reflectance records without ever waiting for UART or cellular I/O.
Every received 5 Hz GPS record enters its FIFO. Calculated reflectance remains
full-rate on SD, while telemetry keeps one latest-value candidate and admits at
most one candidate every 500 ms (2 Hz) to its reliable FIFO. A newer candidate
within the same interval intentionally supersedes the older one. This
rate-limited count is diagnostic, not data-path degradation. The original
ground timestamp and record sequence remain unchanged, so the receiver can
identify exactly which calculated records were selected.

GPS is served first so a reflectance/retry backlog cannot age the flight track.
Every admitted record is retained until a positive application ACK or the
configured freshness deadline, whichever comes first. Production measures the
deadline from local pool admission (not the record's synchronized timestamp)
and expires an entry after 10 seconds. This deliberately trades a small,
explicit gap in live telemetry for a bounded and current backlog during a slow
or disconnected cellular link. Expiration is counted by message type and
reported as mission degradation, but it does not latch infrastructure health
or affect the authoritative SD write. Pool exhaustion likewise drops and
counts only the new live-telemetry copy, then accepts later records once slots
are released. A future SD-backed replay service is required if every selected
record must reach the broker through an arbitrarily long outage.

The task serializes records with the same `data_records` functions used for SD,
calls `telemetry_fragment_plan_init()` once, then
`telemetry_fragment_emit_all()` with a reusable 1024-byte scratch buffer. Its
emitter sends each provided buffer synchronously before returning and never
retains the pointer because the next fragment immediately overwrites it.

Current production policy:

- fragments are written continuously with no application-level idle gap at
  460800 baud. DTF2 framing remains authoritative, while the DTU's configured
  1024-byte/5-ms UART packetizer is free to split or combine MQTT payloads;
- DTU uplink publication uses QoS 1. The DTU remains subscribed to the
  acknowledgement topic at QoS 1, but the ground service publishes each DTA1
  at QoS 0. The application ACK is idempotent and a lost ACK is already covered
  by the B-board timeout/retry and 10-second residency policy, so blocking the
  ground receiver on another broker PUBACK adds load without improving the
  end-to-end acceptance guarantee;
- every accepted 5 Hz A-to-B GPS record is retained and transmitted in FIFO
  order, with GPS ready records scheduled ahead of reflectance/retry backlog;
- new reflectance telemetry is selected on a 500 ms mission-monotonic cadence.
  Each tick admits only the newest unsent calculated record; empty ticks send
  nothing and missed ticks are not replayed as a burst. `MISSION.JSON` reports
  offered, admitted, intentionally rate-limited, and acknowledged counts;
- GPS and reflectance share a 512-entry PSRAM retention pool (roughly 1.6 MiB);
  queue exhaustion is a visible, heartbeat-degrading drop rather than a silent
  overwrite;
- production pool residency is limited to 10 seconds. Staged, queued,
  in-flight, retry-due, and exhausted entries are expired safely; queued
  entries use a tombstone until their FreeRTOS queue pointer is consumed.
  `gps_expired` and `reflectance_expired` expose the resulting live-delivery
  gaps in the heartbeat and `MISSION.JSON`. A late ACK for a released entry is
  harmlessly counted as mismatched;
- the task transmits new messages continuously and does not wait for DTA1
  between messages. Positive ACKs may arrive late, duplicated, or out of order;
- exhausted acknowledgement retries increment `messages_failed` once and
  degrade the heartbeat. The generic component can retain such a slot for a
  late ACK and issue round-robin recovery probes after 30 seconds when its
  residency limit is disabled or configured long enough. With the production
  10-second freshness policy, the old slot expires before that recovery phase;
  live data is preferred to delayed replay. Recovery otherwise runs behind new
  GPS and ordinary retries but ahead of new reflectance, with a global rate
  adapting from 1 to at most 10 messages/s according to pool pressure;
- a matching negative application ACK or a permanent local serialization/
  framing failure releases the slot immediately because retransmitting the same
  immutable payload cannot correct it. DTA1 status 0 means accepted; every
  nonzero status is contractually permanent. A receiver facing temporary
  storage pressure, rate limiting, or another transient condition must withhold
  DTA1 and let the sender's timeout/recovery path retain the record;
- only local infrastructure faults (UART, internal serialization/framing, or a
  pool ownership invariant failure) latch telemetry unhealthy.
  Delivery-pressure counters do not control telemetry or recorder admission.

At 460800 baud, serialization time is now almost entirely the wire time of the
DTF2 bytes; application pacing adds no deliberate delay. Cloud ACK latency is
independent of this serialization path. The 512-entry PSRAM pool absorbs normal
latency and short outages, while the 10-second deadline prevents an old backlog
from consuming it indefinitely. Local SD recording remains the complete,
authoritative data path.

The production UART setting assumes the DTU has already been persistently
provisioned for 460800 baud with `configure_dtu_mqtt.py` and qualified with the
bidirectional path verifier. Transparent mode has no reliable in-band query for
the modem's UART setting. A dead or mismatched uplink is nevertheless visible:
exhausted application-ACK retries increment `messages_failed`, emit a warning,
and produce A-board heartbeat error 5. This detects the failed path but cannot
distinguish baud mismatch from cellular, broker, topic, or receiver failure.

## Cloud acknowledgement

The DTU uplink uses QoS 1, but the ESP32 cannot observe the modem's MQTT PUBACK
and therefore cannot use it to retire retained data. The production
validator/service publishes the 40-byte `DTA1` at MQTT QoS 0 so ACK generation
does not block uplink consumption while waiting for a second broker PUBACK. The
B-board application retry covers an ACK lost on this QoS 0 leg. The
acknowledgement is published on the configured downlink topic only after the
complete DTF2 message and inner DHR1 record pass validation. It echoes the
source ID, mission ID, type, sequence, and complete-message CRC. The ESP32
matches each ACK against every retained in-flight slot. An ACK deadline schedules
the whole logical message for retry without blocking transmission of unrelated
messages. Identical retransmissions are safe because the receiver deduplicates
them and repeats the acknowledgement.

Sender diagnostics distinguish a valid but stale/mismatched ACK from a matching
ACK whose application status is nonzero. `acknowledgement_rejected` remains in
`MISSION.JSON` as the backward-compatible sum of both cases, while
`acknowledgements_mismatched` and `acknowledgements_negative` identify the
cause. Pool occupancy/high-water marks, simultaneous in-flight counts,
transmission attempts, and application-ACK RTT are also checkpointed. A late
positive ACK can release a retry-exhausted or previously rejected entry.

Detailed per-attempt `TX_TIMING` and `ACK_TIMING` logs are controlled by
`CONFIG_DJI_H1_TELEMETRY_TIMING_DIAGNOSTICS`. Project defaults keep them enabled
during field qualification; a quiet production build can disable the option
without changing source code.

At final power-off, telemetry is deliberately abandoned as soon as the B board
receives the `0x30` forecast. Admission closes, both ready queues are cleared
immediately, and the worker reclaims queued and in-flight pool entries. An
in-progress sequence checks cancellation between fragments; only bytes already
accepted by the UART hardware may finish shifting. The abort API never waits
for UART drain, MQTT acknowledgements, or retry deadlines, so cloud delivery
cannot delay authoritative SD finalization. The final `MISSION.JSON` records
`telemetry.shutdown_aborted=true` and `messages_abandoned_shutdown`; this is
intentional shutdown policy, not an infrastructure failure or delivery-pressure
fault.

The `grace_sec` value becomes an absolute B-monotonic cleanup deadline. Reader
and recorder barrier waits are bounded by that deadline, with time reserved for
the final synchronous FAT operations. A FAT flush/close already in progress is
allowed to return because interrupting it could damage the filesystem. The B
board sets `safe_power_off=1` only after all files close and the card unmounts;
deadline expiry alone never claims safety.

Each encoded fragment is submitted to the ESP32 UART in one call. Production
uses no application-level gap between fragments and delegates serial-to-MQTT
packetization to the DTU's configured byte-count/idle-time thresholds. The
receiver validates DTF2 framing and never trusts MQTT publication boundaries as
an integrity mechanism.

### Qualified production rate

The 2026-09-14 controlled bridge tests used the real 98-byte GPS and 2233-byte,
711-sample reflectance payload sizes, GPS at 5 Hz, QoS 1, zero application gap,
and a 460800-baud DTU UART. A 60-second 2 Hz reflectance run followed by a
60-second drain delivered all 300 GPS and 120 reflectance messages; reflectance
p95 latency was 485 ms. A 2.5 Hz bridge run delivered every message, but its p95
latency rose to 1438 ms; a subsequent 10-minute full-stack mission exposed ACK
latency above the 3-second retry deadline and delivered only 537 of 805 selected
reflectance records. At 3 Hz, GPS delivery fell to 83.3% and reflectance to 65%
despite the full drain. Production therefore uses the conservative 2 Hz rate
(one latest candidate every 500 ms) pending long-duration field qualification.

## Receiver contract

`TelemetryFragmentStreamDecoder` first recovers complete `DTF2` fragments when
the DTU splits one fragment across MQTT payloads or combines adjacent fragments.
It searches for the magic, validates declared bounds and CRC, and resynchronizes
after damaged bytes. `TelemetryReassembler` then validates the fragment before
accepting data. It can
receive fragment indices in any order, ignores byte-identical duplicates, and
rejects conflicting duplicates or metadata. It returns a message only after all
indices are present and the complete-message length and CRC have passed.

Incomplete assemblies expire after a bounded timeout. The receiver also bounds
the number of simultaneous assemblies, maximum declared message size, and its
recent-completion deduplication cache, so corrupt or hostile identifiers cannot
grow memory without limit. A completed payload is then passed unchanged to the
existing GPS or reflectance record decoder, which validates the inner `DHR1`
record and its own CRC.
