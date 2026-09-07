# Measurement record format v01

All persistent integers are little-endian. Files begin with a 16-byte `DHF1`
header and contain consecutive `DHR1` records. Every record declares its total
size and ends with an IEEE CRC-32 over all preceding bytes in that record.
`measurement_records.h` is the authoritative field definition; the independent
Python reader in `tests/record_format` is the reference compatibility check.

## Flight directories

The upper 16 bits of the A-board session ID identify a flight. All capture
segments and restart sessions belonging to that flight share `F_XXXX` and keep
continuous file-local record sequences. If the B board reboots and encounters
an existing flight file, it creates `F_XXXX_R01` (then `R02`, etc.) instead of
appending restarted sequences to an old binary stream. Recovery files are
therefore independently decodable and their reboot boundary is unambiguous.

The current production files are `RAW_SPECTRA.BIN` and `REFLECTANCE.BIN`.
Additional v01 record types reserve GPS and operation-log streams for later
milestones.

## Timestamp

Each 60-byte common record header contains B monotonic microseconds, mapped A
monotonic milliseconds, synchronized UTC milliseconds, synchronization age and
generation, synchronization state, and validity flags. Consumers must honor the
validity flags rather than interpreting a canonical zero as a valid value.
`HOLDOVER` timestamps are extrapolated from the last accepted clock model.
Their `sync_age_ms` increases with uncertainty; downstream processing must treat
them as degraded rather than equivalent to freshly synchronized timestamps.

## Measurement interpretation and durability

The recorder synchronizes open measurement files at least every 1.5 seconds
while active and again at every segment boundary. Unexpected power loss can
still truncate or corrupt the current tail record, which readers detect using
record size and CRC, but it should not discard an entire completed mission.

`frame_count` is scoped to one acquisition session and resets on every accepted
START. A must use a new low-16-bit task number for every restart and must not
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
