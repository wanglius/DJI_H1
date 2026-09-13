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
  A slot remains owned until a matching positive application ACK arrives or a
  shutdown abort deliberately discards it.
- The caller of the future MQTT publish API enqueues a complete logical message;
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
one 146-byte fragment. A 3172-byte reflectance record uses four fragments with
wire sizes `1024, 1024, 1024, 292`.

Message-type values intentionally match the existing measurement record types:
GPS `1`, raw spectrum `2`, reflectance `3`, and operation log `4`. The initial
production publisher will send GPS and reflectance only.

## Production sender

The `telemetry` component owns UART1 on GPIO17/GPIO18 and is the only task that
writes application data to the DTU. The measurement recorder gives it finalized
v01 GPS and reflectance records without ever waiting for UART or cellular I/O.
GPS first enters a one-element latest-value mailbox and is admitted to the
shared pool at no more than 1 Hz. Reflectance enters the pool in FIFO order and
is never silently overwritten. Pool exhaustion drops and counts only the new
live-telemetry copy, then accepts later records once acknowledgements release
slots. It does not latch infrastructure health or affect the authoritative SD
write. The loss remains visible to the A board as mission-level data
degradation. A future SD-backed replay service is required if every record must
reach the broker through an arbitrarily long outage.

The task serializes records with the same `data_records` functions used for SD,
calls `telemetry_fragment_plan_init()` once, then
`telemetry_fragment_emit_all()` with a reusable 1024-byte scratch buffer. Its
emitter sends each provided buffer synchronously before returning and never
retains the pointer because the next fragment immediately overwrites it.

Current production policy:

- 6 ms idle gap after every fragment, validated against the current DTU at
  460800 baud;
- DTU uplink publication uses QoS 0 to avoid serializing every 1024-byte
  fragment behind the modem's broker-PUBACK path; the subscribed acknowledgement
  topic remains QoS 1;
- GPS is coalesced to the latest sample and sent no more often than once per
  second;
- GPS and reflectance share a 512-entry PSRAM retention pool (roughly 1.6 MiB);
  queue exhaustion is a visible, heartbeat-degrading drop rather than a silent
  overwrite;
- the task transmits new messages continuously and does not wait for DTA1
  between messages. Positive ACKs may arrive late, duplicated, or out of order;
- exhausted acknowledgement retries increment `messages_failed` and degrade
  the heartbeat, retain the unconfirmed slot for possible late ACK, and do not
  stop later transmissions;
- only local infrastructure faults (UART, internal serialization/framing, or a
  pool ownership invariant failure) latch telemetry unhealthy.
  Delivery-pressure counters do not control telemetry or recorder admission.

At 460800 baud, a current four-fragment reflectance message occupies roughly
74 ms of the UART including its 6 ms boundaries. Cloud ACK latency is typically
hundreds of milliseconds, but no longer consumes the serialization path. The
pool absorbs prolonged latency or outages; it is finite by design so local SD
recording always retains bounded memory behavior.

## Cloud acknowledgement

The DTU uplink deliberately uses QoS 0 because the ESP32 cannot observe an
MQTT PUBACK and therefore cannot use it to retire retained data. The production
validator/service instead publishes a 40-byte
`DTA1` application acknowledgement on the configured downlink topic only after
the complete DTF2 message and inner DHR1 record pass validation. It echoes the
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

At final power-off, telemetry is deliberately abandoned as soon as the B board
receives the `0x30` forecast. Admission closes, the worker reclaims queued and
in-flight pool entries, and an in-progress fragment sequence observes a
cancellation flag. Cloud delivery never delays authoritative SD finalization. The final
`MISSION.JSON` records `telemetry.shutdown_aborted=true`; this is intentional
shutdown policy, not an infrastructure failure or delivery-pressure fault.

The `grace_sec` value becomes an absolute B-monotonic cleanup deadline. Reader
and recorder barrier waits are bounded by that deadline, with time reserved for
the final synchronous FAT operations. A FAT flush/close already in progress is
allowed to return because interrupting it could damage the filesystem. The B
board sets `safe_power_off=1` only after all files close and the card unmounts;
deadline expiry alone never claims safety.

Each encoded fragment should be submitted to the ESP32 UART in one call. After
the UART drains, the sender leaves the configured conservative gap. This often
makes each application fragment a separate MQTT publication. The receiver still
validates framing and does not trust MQTT boundaries as an integrity mechanism.

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
