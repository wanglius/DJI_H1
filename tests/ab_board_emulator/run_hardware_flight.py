"""Run the A-board mission on COM5 and verify real H1 summaries on COM4.

No flashing or physical power removal. --reset explicitly restarts B first.
All output files are exclusively created; ports are released on every exit.
"""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import re
import secrets
import threading
import time

from flight_emulator import FlightEmulator
from ab_board_emulator import encode_frame, CMD_POWER_OFF


def verify_debug(log, report, require_clock=False, require_recording=False):
    failures = []
    if re.search(r'Guru Meditation|Task watchdog got triggered|abort\(\)', log):
        failures.append('MCU panic/watchdog in debug log')
    if len(re.findall(r'ESP-ROM:esp32s3', log)) > 1:
        failures.append('unexpected additional MCU boot')
    for line in log.splitlines():
        if re.search(r'frame errors|reports dropped|overruns|SW drops', line):
            match = re.search(r':\s*(\d+)\s*$', line)
            if match and int(match[1]):
                failures.append(line.strip())
    counts = re.findall(r'H1-([AB]) frames OK\s*:\s*(\d+)', log)
    finals = list(report['session_final_counts'].values())
    if len(counts) != 2 * len(finals) or len(finals) != 2:
        failures.append('missing two real A/B acquisition summaries')
    else:
        for i, expected in enumerate(finals):
            pair = counts[2*i:2*i+2]
            if [p[0] for p in pair] != ['A', 'B'] or any(int(p[1]) == 0 for p in pair):
                failures.append('both H1 channels must produce frames in each session')
            # Protocol status carries completed ground measurements. Sky is a
            # reference input and must not inflate the product frame count.
            if int(pair[0][1]) != expected:
                failures.append('heartbeat count differs from real ground totals')
    if 'Shutdown complete: safe=1 result=ESP_OK' not in log:
        failures.append('missing real successful shutdown/unmount')
    if require_clock:
        if 'State ACQUIRING -> LOCKED' not in log:
            failures.append('clock synchronization never locked')
        if report.get('reconnections'):
            if 'State LOCKED -> HOLDOVER' not in log:
                failures.append('clock synchronization did not enter holdover')
            if not ('State HOLDOVER -> LOCKED' in log or
                    'State INVALID -> LOCKED' in log):
                failures.append('clock synchronization did not relock')
        deltas = [int(value) for value in re.findall(
            r'sync=LOCKED[^\r\n]*utc_delta=(-?\d+)ms[^\r\n]*valid=0x07', log)]
        if not deltas:
            failures.append('no valid synchronized UTC diagnostic')
        elif max(abs(value) for value in deltas) > 250:
            failures.append('synchronized UTC differs from A observation by over 250 ms')
    if require_recording:
        summaries = re.findall(
            r'Segment recorded: raw=(\d+) reflectance=(\d+) dropped=(\d+) '
            r'rejected=(\d+) write_errors=(\d+)', log)
        if len(summaries) != 2:
            failures.append('missing two production recorder summaries')
        else:
            for raw, reflectance, dropped, rejected, errors in summaries:
                if int(raw) <= 0 or int(reflectance) <= 0:
                    failures.append('a segment recorded no raw/reflectance data')
                if int(dropped) != 0:
                    failures.append('production recorder dropped raw data')
                if int(errors) != 0:
                    failures.append('production recorder reported SD write errors')
                if int(rejected) != 0:
                    failures.append('production recorder rejected reflectance data')
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('--debug-port', default='COM4')
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--faults', action='store_true')
    parser.add_argument('--probe', action='store_true', help='command edge cases instead of flight')
    parser.add_argument('--report-prefix', required=True)
    cli = parser.parse_args()
    if cli.port.upper() == cli.debug_port.upper():
        parser.error('protocol and debug ports must differ')
    if cli.probe and cli.faults:
        parser.error('--probe and --faults are separate scenarios')
    args = argparse.Namespace(duration=60, grace=10, scenario='normal',
        session_id=(secrets.randbelow(65535) + 1) << 16 | 1,
        drone_sn='DJI-H1-HARDWARE-MISSION', lost_ack=cli.faults,
        blackout=cli.faults, bad_frames=cli.faults)
    import serial
    stop = threading.Event()
    chunks, capture_errors = [], []
    prefix = Path(cli.report_prefix)
    with ExitStack() as stack:
        report_file = stack.enter_context(prefix.with_suffix('.json').open('x', encoding='utf-8'))
        log_file = stack.enter_context(prefix.with_suffix('.log').open('x', encoding='utf-8'))
        debug = stack.enter_context(serial.Serial(cli.debug_port, 115200, timeout=.1))
        uart = stack.enter_context(serial.Serial(cli.port, 115200, timeout=.01, write_timeout=1))
        debug.dtr = False
        debug.reset_input_buffer()
        uart.reset_input_buffer()

        def capture():
            try:
                while not stop.is_set():
                    raw = debug.read(debug.in_waiting or 1)
                    if raw:
                        # Latin-1 is byte-preserving even if a UTF-8 character
                        # straddles reads; all diagnostic markers are ASCII.
                        text = raw.decode('latin-1')
                        chunks.append(text)
                        log_file.write(text)
                        log_file.flush()
            except Exception as exc:
                capture_errors.append(str(exc))

        thread = threading.Thread(target=capture)
        thread.start()
        try:
            if cli.reset:
                from esptool.reset import HardReset
                HardReset(debug)()
            if cli.probe:
                from control_probe import ControlProbe
                result = ControlProbe(uart, args).run()
            else:
                result = FlightEmulator(uart, args).run()
            time.sleep(1)
        except BaseException:
            # Best-effort safety request, not evidence that shutdown completed.
            uart.write(encode_frame(CMD_POWER_OFF, 251, bytes((10,))))
            raise
        finally:
            stop.set()
            thread.join(2)
        result['hardware_failures'] = verify_debug(
            ''.join(chunks), result, require_clock=True,
            require_recording=True) + capture_errors
        result['passed'] &= not result['hardware_failures']
        json.dump(result, report_file, indent=2)
        print('HARDWARE MISSION:', 'PASSED' if result['passed'] else 'FAILED', flush=True)
        print('Hardware checks:', result['hardware_failures'], flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
