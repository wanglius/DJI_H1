# A-board controlled acquisition milestone

COM5 is the emulated A-board link for development tests; COM4 is the ESP32
debug/flash port. ESP32 TX/RX remain configurable in `main/board_config.h`.

## Ownership and admission

`ab_link.c` parses commands and sends ACKs/1 Hz heartbeats. It never calls H1
or SD operations. `mission_control.c` owns hardware initialization, each
blocking acquisition run, and final SD unmount. The acquisition module retains
its dedicated A reader, B reader, bridge service, and diagnostic logger tasks.

Boot initializes/mounts SD and verifies both H1s, but does not start streams or
write the former COEXIST.BIN scratch file. An accepted start wakes the owner.
ACK success means admission, not completed preparation/stop; the heartbeat is
the authority for physical acquisition state. A stop received during preparation
is latched and cannot be cleared by a delayed worker start. New-session starts
during a run/cleanup return busy. Wait for capture=0 and session=0 before restart.

The owner must be the only caller of acquisition lifecycle APIs. Never call
the old coexistence test concurrently. The timed `acquisition_run_dual()` API
is retained for that standalone bench harness; duration zero is command-driven.

## Session and shutdown semantics

- Same accepted session start: success, no counter reset or repeated action.
- Completed-session start: success, no restart. Restart requires a NEW ID.
- Completed-session stop: success; unknown session stop: state error.
- Boot-local history suppresses the 64 most recently accepted session IDs.
  Older IDs are evicted, so A must not reuse IDs within a flight. History is
  lost on reset; it is not persistent replay protection across power cycles.
- Prepare-power-off is terminal until reset. It immediately abandons queued and
  in-progress telemetry, cancels acquisition, drains/closes the authoritative SD
  files, and unmounts SD. The received grace value is enforced as an absolute
  monotonic deadline for task/barrier waits; repeated requests cannot extend it.
  Safe=1 requires successful cleanup, no latched lifecycle failure, and
  successful unmount. A owns the physical power switch. Software never claims
  safety merely because the grace period expired.
- Raw and reflectance files drain, synchronize, and close before unmount.

## Heartbeat meaning

| Field | Source |
|---|---|
| b_state | 0 during initialization, 1 initialized, 2 fault |
| actual_capture | At least one H1 stream started; held until all streams/readers are stopped |
| frame_count | Successful ground spectra in the current acquisition session |
| session_id | Accepted session during preparation/run/cleanup; zero after completion |
| storage_free_pct | FAT free/total bytes, measured at initialization and after each run |
| safe_power_off | Actual cleanup/unmount completion, never a synthetic timer |

Capacity is cached outside the UART task and refreshed after each run. Error codes are
B-defined: 1=SD initialization/capacity, 2=bridge/H1 initialization,
3=acquisition or cleanup, 4=SD shutdown, 5=frame decode errors in the current run.
Three consecutive read failures stop both readers through normal cleanup.
An isolated failed frame is reported, rather than silently counted as success.
Storage, initialization, and shutdown failures inhibit further starts until
reset. A safely terminated acquisition failure may be retried with a new
session ID; accepting that retry clears error 3. ACK result 1 is only the
protocol's generic failure—the heartbeat error code identifies the subsystem.
Heartbeats continue on failure. Stop latency includes the current
bounded H1 frame read (up to 5 seconds), then stream draining and cleanup.

Recorder pool/queue pressure is deliberately nonfatal. A dropped raw record is
counted, exposes heartbeat error 5, and remains detectable through per-channel
frame-count gaps; the acquisition continues to preserve the rest of the flight.
Persistent write or flush failure remains fatal because the open file can no
longer be trusted.

Navigation payloads now update `drone_data` with the B reception timestamp.
Self-test data is cleared before linking; no sample is available until valid
telemetry arrives. Consumers must still check validity flags and sample age.
No autonomous transport-loss stop policy is added: A sends the documented stop
events and resynchronizes after reconnect.

## Verification

Run the complete mission with `tests/ab_board_emulator/run_hardware_flight.py`.
It exercises start, stop, new-session restart, return/landing stop, and shutdown;
`--faults` adds ACK loss, bad frames, and transport blackout/reconnection. It
checks that both H1s produced frames in both sessions and that final heartbeat
totals equal the hardware summaries. Reports use exclusive files and both ports
are released. SD readback and reflectance/MQTT remain separate milestones.

`--probe` selects command edge cases instead: immediate start/stop cancellation,
session zero, duplicate/old-session replay, conflicting starts, invalid stops,
and power-off while actively acquiring. Use it separately from `--faults`.

### Bench verification, 2026-09-03

ESP-IDF v5.5.5 build/flash succeeded; 21 host tests passed. Normal and combined
fault missions passed. After the final conservative failure-status refinement,
the command probe and combined fault mission both passed on the flashed image:

- Final fault mission: session totals 30 and 8 (A+B), matching H1 summaries.
- Command probe: session totals 8 and 9; immediate cancellation/session-zero
  replay stayed idle; conflicting/invalid commands returned expected ACK codes.
- Active-acquisition power-off reached stopped/safe within about 2 seconds.
- No frame errors, hardware overruns, software drops, or unexpected resets.
- SD free space reported 99%; safe shutdown followed successful unmount.

These runs used approximately 1-second H1 exposures. They do not certify fast
streaming, sensor-disconnect recovery, SD failure recovery, or recorded spectra.
Local evidence is in ignored `build-review/controlled-{normal,faults,probe,final}-20260903`
JSON/debug-log pairs. Hardware-tested firmware SHA-256 (before the review below):
`74f2c9da3ad11005e4c8bdae794fc75d05ed5a2426d08a83959d861c6d561d1a`.

### Pre-commit review

- Shutdown now waits for pending cancelled runs as well as active runs. This
  closes the completion/new-start/power-off interleaving that could unmount SD
  before the queued run performed its final capacity query. Repeated successful
  shutdown requests no longer repeat the unmount-completion log.
- The standalone coexistence harness explicitly arms/reset acquisition state.
- The emulator binds future queued commands to their planned session even if
  scheduling delays make several stages due together. Reconnect still binds to
  the last transmitted target, not a future queued session.
- Stopped heartbeat counts must not decrease from the last live count or change
  after the first stopped report. Three new host regressions cover the emulator
  checks; the full suite now contains 24 tests.

These review changes are build/host-test validated, but have not been reflashed
or rerun on hardware. The preceding bench results apply to the explicitly
identified pre-review image. Ports were not opened during the commit review.
SD recording/folder creation remains a subsequent milestone.
