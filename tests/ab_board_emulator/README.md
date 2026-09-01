# A-board serial emulator

This script emulates the drone-side A board over a 3.3 V USB-to-TTL adapter.
Connect adapter TX to B-board GPIO18 (RX), adapter RX to GPIO17 (TX), and join
the grounds. Do not apply a 5 V UART signal to the ESP32-S3.

The B-board UART link task is not part of the protocol-only milestone yet. Once
it is added, install `pyserial` and run:

```powershell
python tests/ab_board_emulator/ab_board_emulator.py --port COM14
python tests/ab_board_emulator/ab_board_emulator.py --port COM14 --scenario mission
python tests/ab_board_emulator/ab_board_emulator.py --port COM14 --inject-bad-crc
```

The heartbeat scenario performs the handshake, sends fixed navigation data at
5 Hz, and validates the B-board's 1 Hz status reports. The mission scenario
also sends start, stop, and power-off commands, retrying an unacknowledged
command after 200 ms with the original sequence number.

The codec and parser can be checked without hardware or `pyserial`:

```powershell
python tests/ab_board_emulator/ab_board_emulator.py --self-test
```
