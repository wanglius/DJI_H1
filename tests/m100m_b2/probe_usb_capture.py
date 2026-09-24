"""Read-only post-mission USB recovery probe; no reset, commands, or UART TX.

COM11 is observed passively to distinguish loss of debug from MCU inactivity.
COM10 is opened with deasserted DTR/RTS and reopened once if silent for 10 s.
Neither endpoint receives application bytes. USB driver line glitches cannot
be ruled out; retain heartbeats and any boot banners as reset evidence.
"""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests/ab_board_emulator'))
from debug_capture import DiagnosticCapture
from ab_board_emulator import Parser, CMD_STATUS


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--debug-port', default='COM10')
    parser.add_argument('--emulator-port', default='COM11')
    args = parser.parse_args()
    if args.debug_port.upper() == args.emulator_port.upper():
        parser.error('ports must differ')
    args.output.mkdir(parents=True, exist_ok=False)
    chunks, errors, heartbeats = [], [], []
    with ExitStack() as stack:
        log = stack.enter_context((args.output/'usb.log').open('x', encoding='utf-8'))
        health = stack.enter_context((args.output/'capture.jsonl').open('x', encoding='utf-8'))
        debug = serial.Serial(port=None, baudrate=115200, timeout=.1)
        debug.dtr = debug.rts = False
        debug.port = args.debug_port
        debug.open()
        stack.enter_context(debug)
        uart = stack.enter_context(serial.Serial(args.emulator_port, 115200, timeout=.1))
        capture = DiagnosticCapture(debug, log, health, chunks, errors, allow_reopen=True)
        decoder = Parser()
        started = time.monotonic()
        capture.start()
        try:
            while time.monotonic()-started < 25:
                now = time.monotonic()
                for frame in decoder.feed(uart.read(uart.in_waiting or 1), now):
                    if frame.command == CMD_STATUS:
                        heartbeats.append(dict(t=round(now-started, 3), sequence=frame.sequence,
                                               payload_hex=frame.payload.hex()))
        finally:
            result = capture.finish()
    report = dict(capture=result, errors=errors, heartbeats=heartbeats,
                  boot_banners=''.join(chunks).count('ESP-ROM:esp32s3'),
                  reset_requested=False, application_bytes_sent=0)
    (args.output/'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report), flush=True)
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
