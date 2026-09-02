"""Bounded handshake checks on a freshly reset B board (no prior handshake).

Run: python -B tests/ab_board_emulator/test_handshake_hardware.py --port COM5
This sends protocol frames only; it does not start acquisition or reset B.
"""

import argparse
import struct
import time

from ab_board_emulator import (
    CMD_HANDSHAKE, CMD_HANDSHAKE_RESPONSE, CMD_STATUS, Parser, encode_frame,
)


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--port", required=True)
    port = args.parse_args().port
    import serial

    parser = Parser()
    with serial.Serial(port, 115200, timeout=0.02, write_timeout=1) as uart:
        def exchange(sequence, version, link, duration=2.2):
            payload = struct.pack("<BBH32s", version, link, 0x0100, b"")
            uart.write(encode_frame(CMD_HANDSHAKE, sequence, payload))
            frames = []
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                data = uart.read(uart.in_waiting or 1)
                frames.extend(parser.feed(data, time.monotonic()))
            return frames

        def check(condition, label):
            if not condition:
                raise RuntimeError(label)
            print("PASS:", label, flush=True)

        def accepted(frames, sequence):
            replies = [f for f in frames if f.command == CMD_HANDSHAKE_RESPONSE]
            return (len(replies) == 1 and replies[0].sequence == sequence and
                    len(replies[0].payload) == 5 and
                    replies[0].payload[0:2] == b"\x01\x01" and
                    replies[0].payload[4] == sequence)

        # A valid CRC isolates semantic version validation from CRC rejection.
        frames = exchange(200, 2, 1)
        check(not frames, "unsupported version 2 produces no response or heartbeat before linking")
        frames = exchange(201, 1, 2)
        check(not frames, "invalid drone_link=2 does not establish the link")
        frames = exchange(202, 1, 0)
        check(accepted(frames, 202), "version 1 / drone_link=0 / empty SN accepted")
        check(any(f.command == CMD_STATUS for f in frames), "valid handshake starts heartbeats")
        frames = exchange(203, 255, 1)
        check(all(f.command == CMD_STATUS for f in frames),
              "unsupported version 255 gets no handshake response on an active link")
        check(len(frames) >= 2, "existing heartbeat stream survives rejected handshake")
        frames = exchange(204, 1, 1)
        check(accepted(frames, 204), "valid handshake still accepted after rejection")
    print("HANDSHAKE HARDWARE TEST PASSED; serial port released", flush=True)


if __name__ == "__main__":
    main()
