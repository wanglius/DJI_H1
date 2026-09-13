# Hardware tests

These sources are integration harnesses, not production modules. They are not
linked into the production application.

- `coexistence_test.c` runs dual-spectrometer acquisition while continuously
  writing and periodically flushing `COEXIST.BIN` on the SD card. It overwrites
  that file on every run.
- `sd_card_test.c` overwrites `H1TEST.TXT`, then reads it back.
- `hardware_diagnostics/` is a standalone ESP-IDF project. Its Kconfig choice
  selects the destructive SD-card readback test or the dual-H1/SD coexistence
  test. Build it from that directory; the SD test overwrites `H1TEST.TXT`, and
  the coexistence test overwrites `COEXIST.BIN`.
- `h1_single_frame/` is a standalone ESP-IDF project wrapping the original
  single-frame regression diagnostic.
- `dtu_uart_bridge/` is a standalone temporary firmware that bridges the native
  USB Serial/JTAG COM port to the 4G DTU on UART1, GPIO17/GPIO18. It does not
  build into or modify the production application.

Hardware tests may write to the inserted card. Never run them with irreplaceable
media unless the named test files have been backed up.

Deterministic embedded qualification checks live in
`components/startup_checks/`. Enable `CONFIG_DJI_H1_BOOT_SELF_TESTS` only for a
qualification image; normal flight firmware does not compile their test bodies.

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
