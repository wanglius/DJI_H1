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
