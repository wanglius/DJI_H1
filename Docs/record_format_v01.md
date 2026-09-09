# Measurement record format v01

All persistent integers are little-endian. Files begin with a 16-byte `DHF1`
header and contain consecutive `DHR1` records. Every record declares its total
size and ends with an IEEE CRC-32 over all preceding bytes in that record.
`measurement_records.h` is the authoritative field definition; the independent
Python reader in `tests/record_format` is the reference compatibility check.

## Flight directories

The recorder allocates the first unused card-local directory `F_0001` through
`F_9999` during B-board initialization, before it reports ready to A. The
directory remains open through all capture/idle intervals until prepare-power-
off. A-board `session_id` values are opaque 32-bit values: they are stored in
records and compared for command idempotence, but no bits are used for folder
naming or mission grouping. A B-board reboot allocates another unused directory
and therefore never appends restarted sequences to an old binary stream.

Each flight directory contains:

- `RAW_SPECTRA.BIN`: ground and sky H1 frames.
- `REFLECTANCE.BIN`: calculated reflectance records.
- `GPS_TRACK.BIN`: every accepted A-board realtime/geolocation sample received
  after the ready handshake, including samples before, during, and between
  capture segments.
- `EVENTS.JSONL`: append-only structured lifecycle and fault events. Each line
  is an independent JSON object so a damaged tail does not invalidate earlier
  events.
- `MISSION.JSON`: a human-readable checkpoint containing identity, firmware,
  timing, aggregate record/drop/error counts, and mission state. It is replaced
  through `MISSION.TMP`; an interrupted replacement may leave `MISSION.BAK` as
  the recoverable prior checkpoint.

The protocol intentionally provides no B-readable flight key: section 4.9 says
that `session_id` is opaque. The B boot-to-poweroff lifecycle therefore defines
the local flight directory. Handshake identity is cached into `MISSION.JSON`
and the accepted handshake is the first normal `EVENTS.JSONL` lifecycle event.

## Timestamp

Each 60-byte common record header contains B monotonic microseconds, mapped A
monotonic milliseconds, synchronized UTC milliseconds, synchronization age and
generation, synchronization state, and validity flags. Consumers must honor the
validity flags rather than interpreting a canonical zero as a valid value.
`HOLDOVER` timestamps are extrapolated from the last accepted clock model.
Their `sync_age_ms` increases with uncertainty; downstream processing must treat
them as degraded rather than equivalent to freshly synchronized timestamps.

## Measurement interpretation and durability

The recorder synchronizes all open flight files at least every 1.5 seconds and
again at every segment boundary. Unexpected power loss can
still truncate or corrupt the current tail record, which readers detect using
record size and CRC, but it should not discard an entire completed mission.

`frame_count` is scoped to one acquisition session and resets on every accepted
START. A must use a new 32-bit session ID for every restart and must not
reuse recent session IDs. Gaps in raw frame counts indicate intentional drops
caused by bounded recorder backpressure; acquisition continues so the remainder
of the flight can still be recovered.

Reflectance values are stored in 0.01 percent units and deliberately clamped to
0–100%. Per-sample flags preserve whether clamping or an invalid denominator
occurred, while the raw spectra retain the original signal for offline science.
The current exposure/scale normalization is an engineering assumption pending
a controlled same-target, multiple-exposure validation of the H1 response.

### Backpressure verification, 2026-09-07

A test-only build delayed the first periodic flush by 8 seconds. Both H1 tasks
and the 1 Hz heartbeat continued: the recorder queue reached its 24-frame
capacity, four raw frames were explicitly dropped, and heartbeat error 5 made
the degraded dataset visible. The writer then recovered, recorded the second
segment without loss, flushed and closed both files, and completed SD unmount
with `safe=1`. The production build was restored afterward with injection set
to its default zero value.
