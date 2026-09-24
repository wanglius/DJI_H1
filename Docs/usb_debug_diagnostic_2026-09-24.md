# USB debug silence diagnostic — 2026-09-24

## Scope and conclusion

Board native USB: COM10; A-board emulator: COM11. Production firmware was
neither edited nor rebuilt/reflashed for this diagnostic. Tests used the
existing M100M-B2 firmware and unchanged MQTT receiver/ACK logic.

**The host capture thread is not stuck in a serial read in these runs.**
Reads complete repeatedly in about 100–125 ms but return no data. The board
continues responding over COM11 and producing valid MQTT records. Closing
and reopening COM10 during an active mission did not restore output.

This narrows the failure to the USB logging path (Windows driver/endpoint,
USB hardware/link, or MCU console state). It does **not** identify the exact
root cause or prove that the firmware console is healthy. Neither a Python
exception nor a file-write stall explains the observed zero-byte reads.

The original 10-minute flight stopped logging at MCU time 165216 ms. Both
new missions were already silent from their first read, so they do not
reproduce the transition from working to silent or implicate segment stop.

## Instrumentation

- `tests/ab_board_emulator/debug_capture.py` records thread liveness, current
  operation and duration, completed/empty read counts, bytes, last-data age,
  maximum read duration and observed inter-read data gap, and recovery events.
- `run_hardware_flight.py --capture-diagnostics` creates a separate
  `.capture.jsonl` sidecar and adds final capture state to the mission JSON.
- `--reopen-on-debug-silence` permits one reopen after 10 seconds without
  bytes, including silence from initial open. Only the reader closes/opens
  the handle; a long-blocked read can be cancelled by the observer.
- Reopen preserves DTR/RTS and sends no application bytes or explicit reset.
  Driver-generated line glitches cannot be categorically excluded. Boot
  banners and independent protocol continuity must be checked, not assumed.
- An attempted recovery remains a diagnostic failure in the overall report,
  even if later output recovers. Missing log evidence is never a full pass.
- Instrumentation is opt-in; default flight behavior is unchanged.

Five host-only capture tests cover normal reads, repeated empty reads,
blocked-read cancellation, silence from initial open, and read exceptions.
All 34 emulator/capture tests passed:

```powershell
python -B -m unittest discover -s tests/ab_board_emulator -p 'test_*.py'
```

## Hardware evidence

All result directories below are under `tests/m100m_b2/results/` and ignored
by Git. Broker QoS1 loopback and receiver SUBACK preceded each mission.

| Evidence | Four-minute plan | One-minute confirmation |
|---|---:|---:|
| Directory | `usb-diagnostic-20260924-02` | `usb-diagnostic-20260924-03` |
| Capture duration | 229.266 s | 60.266 s |
| Completed serial reads | 2081 | 547 |
| Empty reads | 2081 | 547 |
| USB bytes received | 0 | 0 |
| Maximum read duration | 125 ms | 125 ms |
| Reopen attempts | 0 | 1 |
| A-board protocol failures | 0 | 0 |
| Heartbeats received | 227 | 58 |
| Ground frame counts per segment | 61, 13 | 14, 2 |
| MQTT complete messages | 202 | 59 |
| Decoded GPS records | 1125 | 275 |
| Decoded reflectance records | 74 | 16 |
| Decoded event records | 13 | 13 |
| Ground decode/journal errors | 0 | 0 |
| Final heartbeat `safe` | 1 | 1 |

The four-minute plan terminates early on acknowledged safe shutdown, as in
the normal emulator scenario. Its first instrumented version only triggered
recovery after at least one received byte; it therefore did not reopen.
That diagnostic limitation was corrected and regression-tested before the
one-minute confirmation (no production changes).

During the confirmation, silence was detected at 10.078 s and reopen
completed at 10.172 s with DTR=false and RTS=false. COM10 remained silent
through the rest of the active mission. Both capture workers exited normally.
COM11 continued heartbeats without a reported reconnection/protocol failure;
the final heartbeat at 59.047 s had error=0, capture=0, session=0, safe=1.
No USB boot banners were available to independently exclude a reset.

An intermediate passive probe (`usb-reopen-20260924-01`) opened/reopened
COM10 after shutdown, observing 234 empty reads over 25.094 seconds. COM11
also emitted no heartbeats then, so **that probe alone is inconclusive**:
a shut-down application can legitimately be quiet. The active confirmation
above is the meaningful recovery test.

`usb-diagnostic-20260924-01` contains a preflight socket-permission failure;
no hardware mission was launched in that attempt.

Both full hardware verdicts remain failed because USB acquisition summaries,
clock diagnostics and SD-unmount logs are unavailable. Protocol safe=1 and
MQTT records are independent positive evidence, not a substitute for SD
readback or the missing debug validations. Ground ACK publication counts
also do not prove every ACK was consumed by the MCU.

## Next isolation step

1. The last mission reached safe=1; all test processes finished and released
   COM10/COM11. No flashing, commit, or production configuration change was made.
2. Physically unplug/reconnect the board's native USB cable to force USB
   re-enumeration. If separately powered, leave its other supply unchanged
   to separate USB recovery from a complete power cycle where practicable.
3. Confirm the new COM number and run an instrumented short mission again.
4. If this restores logs, repeat across start/stop boundaries and beyond the
   original 165-second cutoff. If not, investigate console state and USB
   electrical/driver behavior before altering firmware.

Espressif's console implementation has bounded TX waiting and may drop bytes
when the host cannot drain its buffer. This is a possible mechanism, not a
diagnosis of the trigger. Reference: installed ESP-IDF 5.5.5
`components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c`, and
[USB Serial/JTAG console documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/usb-serial-jtag-console.html).
