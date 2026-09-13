# Hardware tests

These sources are integration harnesses, not production modules. The active
milestone firmware selects them explicitly from `main/CMakeLists.txt`.

- `coexistence_test.c` runs dual-spectrometer acquisition while continuously
  writing and periodically flushing `COEXIST.BIN` on the SD card. It overwrites
  that file on every run.
- `sd_card_test.c` overwrites `H1TEST.TXT`, then reads it back.
- `data_pipeline_test.c` validates the documented 30-byte drone payload decoder
  and common in-memory record metadata.
- `ab_protocol_test.c` validates A-B CRC, complete-frame encoding, incremental
  parsing, timeout recovery, CRC rejection, and status-payload round trips.
- `ab_link_test.c` is a simulated B-board endpoint on GPIO17/GPIO18 for the
  PC-side A-board emulator. Its mission state and storage percentage are fake.
- `h1_single_frame_test.c` is the original standalone `app_main` retained for
  regression diagnosis; it is not part of the normal build.
- `dtu_uart_bridge/` is a standalone temporary firmware that bridges the native
  USB Serial/JTAG COM port to the 4G DTU on UART1, GPIO17/GPIO18. It does not
  build into or modify the production application.

Hardware tests may write to the inserted card. Never run them with irreplaceable
media unless the named test files have been backed up.

## Host-only tests

Run the authoritative repository host-test entry point from the project root:

```powershell
python -B tests/run_host_tests.py
```

The runner discovers every immediate `tests/*/test_*.py` group, rejects an
empty group, and currently covers the mission viewer, A-board emulator, record
format, and telemetry transport. These tests do not require hardware or access
an SD card. Do not use plain `python -m unittest discover -s tests`: the nested
test directories are intentionally not Python packages, so that command can
silently discover zero tests.
