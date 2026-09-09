# Complete A-board flight emulator

Based on the full `Docs/AB板串口通信协议_V1.0.md` document. This is a separate
runner; the original short `ab_board_emulator.py` remains available unchanged.

## Run on this workstation

Connect USB-UART TX to ESP32 GPIO44, RX to GPIO43, and common ground (3.3 V TTL).
COM5 is the USB-UART bridge; COM4 is the ESP32 flash/debug port. This program
opens **only COM5**, does not flash/reset B, and never physically switches power.

```powershell
& 'C:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe' -u -B tests/ab_board_emulator/flight_emulator.py
```

On any Python installation with `pyserial`, `python -B` is sufficient.
`--port COM5` is the default and can be changed. Each run generates a random
nonzero 16-bit power-up identifier with task number 1. Use `--session-id 0x12340001`
to reproduce a session. Do not reuse a completed session ID when expecting a
fresh capture: B intentionally treats that as an idempotent replay.

```powershell
python -B tests/ab_board_emulator/flight_emulator.py --lost-ack --blackout --bad-frames
python -B tests/ab_board_emulator/flight_emulator.py --scenario low-battery
python -B tests/ab_board_emulator/flight_emulator.py --scenario manual-abort
python -B tests/ab_board_emulator/flight_emulator.py --scenario drone-link-loss
python -B tests/ab_board_emulator/flight_emulator.py --duration 120 --report flight-report.json
python -B tests/ab_board_emulator/flight_emulator.py --endurance --report endurance-report.json
```

Reports are exclusively created, never overwritten. Console output is a JSON
event timeline (stages, commands, ACKs, heartbeats, failures). Exit code 0 means
the checks passed; failures produce a nonzero exit. Exceptions/interrupts close
the serial port, but do not imply B has stopped or flushed data.

## Default 60-second mission

Times are relative to successful initial handshake. Mission motion is compressed;
protocol timing remains real: navigation 5 Hz, handshake retry 1 Hz, ACK timeout
200 ms, heartbeat loss 3 s. `--duration` scales stages only (minimum 60 seconds).

| Time | Event |
|---|---|
| 0 | Handshake with drone_link=0 and empty serial; begin navigation |
| 2.4 s | Drone connected, serial-number supplement, preflight |
| 6 s | Motors running on ground |
| 8.4 s | Automatic takeoff (mode 11), climb |
| 14.4 s | Transit at 120 m |
| 18 s | Survey: start command, same session throughout |
| 33 s | Normal survey stop, reason 1 |
| 38.4 s | Restart acquisition with a new session ID |
| 42 s | Return-home (mode 15), unconditional stop reason 2 |
| 48 s | Automatic landing (mode 12), stop reason 4 |
| 54 s | Landed/motors stopped, stop reason 5 |
| 55.2 s | Preparing power-off, stop reason 8 |
| 56.4 s | Prepare-power-off command, default 10-second grace |

The runner ends once a successful power-off ACK and a stopped/session-zero/
safe heartbeat are seen. Otherwise it simulates the grace-expiry power-cut
decision and reports failure. It never toggles power. Overall time is bounded
by duration + grace + 20 seconds, including failure to establish a link.

Alternative scenarios change the return-home stop reason to 3 (low battery),
6 (manual abort), or 7 (drone link loss). They occur after survey completion in
this version; they are not mid-survey emergency flight trajectories. Low battery
changes the battery data, and drone-link loss clears data validity and A-status.

## Ten-minute endurance mission

`--endurance` fixes the duration at 600 seconds and enables a deterministic,
field-shaped test. It flies four alternating 400 m north/south survey lines,
spaced 50 m eastward, at 120 m altitude. Every line receives a fresh session ID
and independent START/STOP pair, producing four recorder segment summaries.
Latitude and longitude are transmitted at 5 Hz in the normal 30-byte realtime
payload and interpolated in a local tangent plane around 39.9042 N, 116.4074 E.
Stage events in the JSON report include decoded coordinates and altitude.

The plan injects lost action ACKs, one bad telemetry CRC, one truncated telemetry
frame, a 4.5-second UART blackout/reconnect during line 2, and a temporary RTK
quality degradation during line 3. Navigation continues with a GPS-quality fix
during the RTK event. Return, landing, recorder shutdown, SD unmount, and the
safe-power-off heartbeat remain mandatory.

| Mission time | Event |
|---|---|
| 0–48 s | Handshake, preflight, takeoff, climb and transit |
| 72–162 s | Test line 1, south to north |
| 180–258 s | Test line 2, north to south; link blackout near 184 s |
| 276–354 s | Test line 3, south to north; RTK degraded at 300–324 s |
| 372–450 s | Test line 4, north to south |
| 468–588 s | Return, landing and safe shutdown sequence |

## Protocol coverage and assumptions

- Explicit little-endian field encoding; shared frame encoder/parser and CRC.
- Navigation continues during idle, capture, landing and shutdown negotiation.
- Smooth synthetic location/altitude, real UTC, A uptime with uint32 wraparound.
- Takeoff uses GPS. After RTK fixes, position switches to RTK but altitude remains
  GPS (`src_flags=1`, no RTK takeoff-height bit). Validity/source bits are separate.
- A does not equate ACK with actual acquisition: every heartbeat is checked
  against target capture/session state with a 3-second transition allowance.
  Persistent mismatch fails and triggers state resynchronization.
- Frame counts must not decrease within the active session. B errors, invalid
  status fields, and capture+safe contradictions fail the run.
- A single pending action is retried using identical command/SEQ/payload.
  “最多3次重发” is interpreted as **initial transmission plus three retries**;
  confirm this interpretation with the A-board team (the older short test uses
  three total attempts). Successfully completed duplicate ACKs are tolerated.
- Heartbeat spacing tolerance is 0.75–1.25 s at the PC, not a precision timing
  certification; sequence gaps outside injected outages fail.
- `--lost-ack` discards the first successful ACK of each action.
- `--blackout` discards inbound bytes and pauses outbound traffic for 4.5 seconds
  during survey. A detects heartbeat loss, performs 1 Hz handshake reconnect,
  then sends a new-SEQ command for the existing desired session. This emulates
  a transport outage, not an ESP32 reset or physical cable disconnect.
- `--bad-frames` corrupts one navigation CRC and truncates another navigation
  frame. Continuing mission verifies recovery, **not** that B consumed every
  valid navigation field; the protocol has no telemetry ACK/readback.

The B endpoint now controls real H1 acquisition. `frame_count` is the sum of
successfully decoded A+B spectra, not matched pairs or recorded frames. It is
reset for a new session, retained after stop, and compared with both H1 debug
summaries by `run_hardware_flight.py`. Restart uses a new task number; replaying
a completed session must never restart it. Shutdown is terminal until B resets.
SD free space is measured, and safe-power-off requires successful acquisition
cleanup and SD unmount. There is no spectral recorder yet, so this is not proof
of spectral data persistence. The current mode creates no SD files.

To capture COM4 and compare the real counts while driving COM5:

```powershell
python -B tests/ab_board_emulator/run_hardware_flight.py --reset --report-prefix build-review/mission-normal
python -B tests/ab_board_emulator/run_hardware_flight.py --reset --faults --report-prefix build-review/mission-faults
python -B tests/ab_board_emulator/run_hardware_flight.py --reset --probe --report-prefix build-review/mission-probe
python -B tests/ab_board_emulator/run_hardware_flight.py --reset --endurance --report-prefix build-review/mission-endurance
```

`--reset` restarts B explicitly, without flashing or power removal. Use a new
report prefix each run. This runner needs pyserial and esptool for reset.
Without `--reset`, start it before manually resetting B so all summaries are
captured. COM4 is the debug port and COM5 the emulator port unless overridden.

## Tests and module boundaries

`flight_model.py` provides deterministic flight data. `flight_emulator.py`
contains the command/state machine, bounded UART loop and report. Existing
`ab_board_emulator.py` supplies the codec. No new third-party dependency beyond
`pyserial` is needed for hardware; host tests require only the standard library.

```powershell
python -B -m unittest discover -s tests/ab_board_emulator -p "test_*.py"
```

`test_flight.py` uses a virtual clock and a Python fake B-board to exercise full
normal/fault missions quickly. It does not execute or replace testing the ESP32 C
firmware. The older hardware handshake script runs only when invoked explicitly.
