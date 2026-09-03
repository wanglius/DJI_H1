# A-board serial emulator

For a complete takeoff/survey/return/landing mission on COM5, see
[the flight emulator guide](FLIGHT_EMULATOR.md). The script below remains the
short milestone regression test.

This script emulates the drone-side A board over a 3.3 V USB-to-TTL adapter.
Connect adapter TX to B-board GPIO18 (RX), adapter RX to GPIO17 (TX), and join
the grounds. Do not apply a 5 V UART signal to the ESP32-S3.

The current B-board firmware controls real acquisition and reports real status.
It does not yet record spectra. Use the complete hardware runner above for
start/stop/restart and debug-count verification. The older short runner below
is retained for protocol regression. Install `pyserial` and run:

```powershell
python tests/ab_board_emulator/ab_board_emulator.py --port COM5
python tests/ab_board_emulator/ab_board_emulator.py --port COM5 --scenario mission --simulate-lost-ack
python tests/ab_board_emulator/ab_board_emulator.py --port COM5 --inject-bad-crc
```

The heartbeat scenario performs the handshake, sends fixed navigation data at
5 Hz, and validates the B-board's 1 Hz status reports. The mission scenario
also sends start, stop, and power-off commands, retrying an unacknowledged
command after 200 ms with the original sequence number. It repeats start at
6 seconds with a new SEQ and the same session, checking that the frame count
does not reset. `--simulate-lost-ack` ignores the first successful ACK for each
action to force a same-SEQ retry. Use at least 20 seconds for the mission test.

The emulator returns exit code 1 on validation failure, or 0 on success.
Mission success requires ACKs and heartbeats confirming capture, stopped
(session=0), and safe power-off states. The short runner alone cannot compare
counts against the real H1 logs. Neither runner physically removes power.
Reset B before another mission after it has acknowledged safe shutdown.

The codec and parser can be checked without hardware or `pyserial`:

```powershell
python tests/ab_board_emulator/ab_board_emulator.py --self-test
python -B -m unittest discover -s tests/ab_board_emulator -p test_emulator.py
```

The second command also checks emulator pass/fail behavior, lost-ACK handling,
counter-reset detection, and session clearing without opening a serial port.

After flashing/resetting B, and before running the normal emulator, verify
handshake rejection on hardware with:

```powershell
python -B tests/ab_board_emulator/test_handshake_hardware.py --port COM5
```

This bounded test sends valid-CRC unsupported-version requests before and
after linking, checks invalid `drone_link=2`, and verifies that version 1
with `drone_link=0` and an empty serial number is accepted. B must not already
have completed a handshake when this test starts. The port is closed on exit.
