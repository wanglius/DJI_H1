# Emulator flight-test SOP

## 1. Purpose and release gate

This procedure verifies the production B-board firmware with the A-board flight
emulator, both H1 spectrometers, SD recording, synchronized navigation, and the
4G DTU/MQTT telemetry path.

The MQTT ground validator is part of the test system, not optional monitoring.
It validates complete `DTF2`/`DHR1` records and publishes the `DTA1`
application acknowledgements that release entries from the B-board telemetry
retention pool. MQTTX can display traffic, but it does not replace the
validator.

**No-go rule:** do not reset the B board, start the A-board emulator, or begin a
simulated flight until the validator prints `TELEMETRY READY`. If the validator
exits at any time, do not launch. If it exits after launch, mark the test invalid
even if the aircraft simulation and SD recording finish safely.

## 2. Standard bench arrangement

Current custom-board wiring:

| Function | Connection |
|---|---|
| ESP32 flash and diagnostic log | COM6, native USB Serial/JTAG |
| Emulated A-board UART | COM5 USB-UART bridge |
| B-board UART0 TX/RX | GPIO43 / GPIO44 |
| DTU UART TX/RX, MCU side | GPIO17 / GPIO18, 460800 baud |
| H1-A | SC16 channel A, ground-looking spectrum |
| H1-B | SC16 channel B, sky-looking spectrum |

COM numbers are workstation assignments, not firmware constants. Confirm them
in Device Manager after reconnecting hardware. The USB-UART bridge must use
3.3 V logic and share ground with the B board.

Before applying power, confirm:

- the SD card is inserted and has sufficient free space;
- both spectrometers, the DTU, its antenna, and the SIM are connected;
- the DTU and board have a stable power supply suitable for cellular bursts;
- no serial terminal, IDF monitor, or stale Python process owns COM5 or COM6;
- the test operator has selected a new, unique report prefix.

## 3. Terminal layout

Use three separate ESP-IDF/PowerShell terminals and keep all three visible:

1. **Ground validator** — connects to EMQX, receives telemetry, and sends DTA1
   acknowledgements.
2. **Flight runner** — drives COM5 and captures/validates the COM6 firmware log.
3. **Utilities** — build, flash, broker probe, and post-test checks.

MQTTX may remain open as an additional observer. Give every MQTT client a unique
client ID; the supplied validator generates unique IDs automatically.

All commands below run from the repository root. Use the Python interpreter in
the active ESP-IDF environment, or another Python installation with `pyserial`.

## 4. Build and flash the production firmware

Record the Git revision and whether the worktree contains uncommitted test
changes:

```powershell
git rev-parse --short HEAD
git status --short
```

Build and flash. Do not leave an IDF serial monitor attached afterward because
the flight runner must open COM6 itself.

```powershell
idf.py -B build-review build
idf.py -B build-review -p COM6 flash
```

A successful flash is not permission to launch; complete the ground-system gate
next.

## 5. Ground-system pre-flight gate

### 5.1 Verify the broker independently

Run the broker loopback before starting the telemetry validator. The probe sends
one disposable message on the uplink test topic; running it first prevents that
non-DTF2 payload from entering the validator's stream.

```powershell
python -B tests/dtu_uart_bridge/mqtt_broker_probe.py `
  --host mqtt-mgnt.torchbearer.tech --port 1883 `
  --username DJI_H1_001 --topic dji-h1/test/up --qos 1
```

Required result:

```text
MQTT PASS host=mqtt-mgnt.torchbearer.tech:1883 topic=dji-h1/test/up qos=1
```

If this command fails, the test is **NO-GO**. Check DNS, Internet/cellular
connectivity, broker availability, authentication, and topic permissions before
continuing.

### 5.2 Start the production telemetry validator

For a ten-minute endurance mission, start this in the **Ground validator**
terminal:

```powershell
python -u -B tests/dtu_uart_bridge/monitor_telemetry.py `
  --host mqtt-mgnt.torchbearer.tech --mqtt-port 1883 `
  --username DJI_H1_001 `
  --topic dji-h1/test/up --ack-topic dji-h1/test/down `
  --duration 720 --expect-qos 1 --ack-qos 0 `
  --expect-gps-min 2900 --expect-reflectance-min 300
```

For a normal 60-second regression mission, use a 120-second validator window
and conservative minimums:

```powershell
python -u -B tests/dtu_uart_bridge/monitor_telemetry.py `
  --host mqtt-mgnt.torchbearer.tech --mqtt-port 1883 `
  --username DJI_H1_001 `
  --topic dji-h1/test/up --ack-topic dji-h1/test/down `
  --duration 120 --expect-qos 1 --ack-qos 0 `
  --expect-gps-min 200 --expect-reflectance-min 40
```

The validator connects two clients, waits for the uplink SUBACK, and pings both
connections. Uplink measurement records remain QoS 1. DTA1 acknowledgements
use QoS 0 because they are idempotent and the B board retries a record when its
application ACK is lost; this prevents broker PUBACK latency from blocking the
receive loop. Only then does the validator print:

```text
TELEMETRY READY host=mqtt-mgnt.torchbearer.tech:1883 topic=dji-h1/test/up ack_topic=dji-h1/test/down uplink_qos=1 ack_qos=0
```

This exact line is the ground-system readiness indication. Confirm that the
process is still running, then proceed immediately to the flight runner. An open
terminal without this line is not ready. `TELEMETRY INVALID`, a traceback, a
returned prompt, or any nonzero exit code is **NO-GO**.

The validator duration starts after readiness. Its endurance allowance includes
board reset, H1 preparation, the 600-second mission, and final telemetry drain.

## 6. Launch the emulator mission

The validator must already show `TELEMETRY READY` before executing either
command in this section.

Use a unique prefix; the runner intentionally refuses to overwrite an existing
`.json` or `.log` report.

Ten-minute, four-line survey with deterministic mid-air incidents:

```powershell
python -u -B tests/ab_board_emulator/run_hardware_flight.py `
  --port COM5 --debug-port COM6 --reset --endurance `
  --report-prefix build-review/mission-YYYYMMDD-endurance-01
```

Normal 60-second regression mission:

```powershell
python -u -B tests/ab_board_emulator/run_hardware_flight.py `
  --port COM5 --debug-port COM6 --reset `
  --report-prefix build-review/mission-YYYYMMDD-normal-01
```

Starting this command is the simulated launch sequence: it opens both ports,
resets B, performs the handshake, streams 5 Hz navigation, controls acquisition,
and requests safe shutdown. Do not flash, reset, or open either COM port from a
different program during the run.

## 7. In-flight monitoring and abort handling

During the mission, verify that:

- the validator continuously prints `GPS` records and prints `REFLECTANCE`
  records during acquisition segments;
- the flight runner continues to receive 1 Hz heartbeats;
- there is no `TELEMETRY INVALID`, MCU panic, watchdog, or unexpected reboot;
- a temporary telemetry backlog recovers instead of growing without bound;
- expected endurance incidents are reported by the emulator and recovered.

Do not judge telemetry health from MQTTX alone. A message visible in MQTTX has
reached that subscriber, but it has not necessarily passed DTF2/DHR1 validation
or received a DTA1 acknowledgement.

If the ground validator stops during flight:

1. declare the qualification run failed and record the time/cause;
2. restart the validator if possible so acknowledgements resume and the mission
   can reach a controlled shutdown;
3. do not reinterpret the recovered run as a clean pass;
4. preserve both terminal outputs and the runner artifacts for diagnosis.

If the flight runner is interrupted, it sends a best-effort power-off request,
but that is not proof that files were flushed or the SD card was unmounted. Keep
power applied until the firmware reports successful safe shutdown whenever
possible.

## 8. Completion criteria

Allow the validator to continue after the flight runner finishes so late
fragments and acknowledgements can drain. Do not close it manually before its
summary.

The flight side passes only when it ends with:

```text
HARDWARE MISSION: PASSED
Hardware checks: []
```

Its `.log` must also contain the successful shutdown/unmount marker, acquisition
summaries for both H1 channels, and recorder summaries with no unexpected SD
write/flush errors or production drops.

The ground side passes only when:

- it prints one `TELEMETRY SUMMARY`;
- `gps` and `reflectance` meet the selected minimums;
- it prints no `TELEMETRY INVALID`;
- the process exits with code 0 (`$LASTEXITCODE -eq 0` in PowerShell).

The validator's `gps` and `reflectance` values count unique, fully reassembled,
CRC-validated records received on the ground. Publishing a DTA1 confirms ground
acceptance; it does not by itself prove that the downlink ACK reached B. Compare
these counts with the firmware telemetry status and SD record totals when
investigating losses.

A nominal run requires **both** the flight side and ground side to pass. SD
success alone cannot compensate for failed live telemetry, and telemetry success
alone cannot compensate for an unsafe or incomplete recorder shutdown.

## 9. Artifacts and test record

Retain at least:

- the tested Git commit and `git status --short` output;
- firmware build result and binary size;
- flight-runner `.json` and `.log` files under `build-review/`;
- validator command, readiness line, final summary, and exit code;
- broker/username/topic names, board serial, SD card identifier, date, operator,
  illumination/environment, and any deviations from this SOP.

The build and log directories are intentionally ignored by Git. Copy formal
qualification evidence to the team's controlled test-record location rather
than committing generated logs to the source repository.

## 10. Quick go/no-go checklist

- [ ] Hardware, antenna, SIM, SD card, and both H1 units are connected.
- [ ] COM5 and COM6 are correct and free.
- [ ] Production firmware builds and flashes successfully.
- [ ] Broker probe prints `MQTT PASS`.
- [ ] Validator command matches production host, topics, and QoS.
- [ ] Validator prints `TELEMETRY READY` and remains running.
- [ ] Only now: start the flight runner/reset B.
- [ ] Flight runner prints `HARDWARE MISSION: PASSED` and no hardware failures.
- [ ] Validator prints a passing `TELEMETRY SUMMARY` and exits with code 0.
- [ ] Safe shutdown/unmount is present and all artifacts are retained.
