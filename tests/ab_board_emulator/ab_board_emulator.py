#!/usr/bin/env python3
"""PC-side emulator for the DJI_H1 drone-side A board."""

from __future__ import annotations

import argparse
import struct
import sys
import time
from dataclasses import dataclass

HEAD = b"\xAA\x55"
MAX_PAYLOAD = 247
INTERBYTE_TIMEOUT_S = 0.100
# Section 5.3: one initial transmission followed by at most three retries.
ACTION_MAX_ATTEMPTS = 4

CMD_REALTIME = 0x01
CMD_START = 0x10
CMD_STOP = 0x11
CMD_HANDSHAKE = 0x20
CMD_POWER_OFF = 0x30
CMD_STATUS = 0x81
CMD_ACK = 0x90
CMD_HANDSHAKE_RESPONSE = 0xA0


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frame(command: int, sequence: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload exceeds protocol maximum")
    body = bytes((len(payload), command & 0xFF, sequence & 0xFF)) + payload
    return HEAD + body + struct.pack("<H", crc16_ccitt_false(body))


@dataclass(frozen=True)
class Frame:
    command: int
    sequence: int
    payload: bytes


class Parser:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.last_byte_at: float | None = None

    def feed(self, data: bytes, now: float) -> list[Frame]:
        frames: list[Frame] = []
        if self.buffer and self.last_byte_at is not None and now - self.last_byte_at > INTERBYTE_TIMEOUT_S:
            # Discard only the stale partial frame; incoming data below is
            # scanned afresh for AA 55, including a trailing split AA byte.
            self.buffer.clear()
        if data:
            self.buffer.extend(data)
            self.last_byte_at = now
        while True:
            header = self.buffer.find(HEAD)
            if header < 0:
                self.buffer[:] = self.buffer[-1:] if self.buffer.endswith(HEAD[:1]) else b""
                break
            if header:
                del self.buffer[:header]
            if len(self.buffer) < 3:
                break
            payload_length = self.buffer[2]
            if payload_length > MAX_PAYLOAD:
                del self.buffer[0]
                continue
            frame_length = payload_length + 7
            if len(self.buffer) < frame_length:
                break
            candidate = bytes(self.buffer[:frame_length])
            received_crc = struct.unpack_from("<H", candidate, frame_length - 2)[0]
            expected_crc = crc16_ccitt_false(candidate[2:-2])
            if received_crc != expected_crc:
                print(f"{stamp()}  RX bad CRC expected=0x{expected_crc:04X} got=0x{received_crc:04X}")
                # Slide by one byte and scan again. A truncated/corrupt frame
                # may have consumed AA or AA 55 from the next valid frame as
                # its CRC, so deleting the entire candidate would mask the
                # same recovery failure in the firmware under test.
                del self.buffer[0]
                continue
            del self.buffer[:frame_length]
            frames.append(Frame(candidate[3], candidate[4], candidate[5:-2]))
        return frames


def stamp() -> str:
    return f"{time.monotonic():12.3f}"


def handshake_payload(serial_number: str) -> bytes:
    serial_bytes = serial_number.encode("ascii", errors="strict")[:32]
    return struct.pack("<BBH32s", 1, 1, 0x0100, serial_bytes)


def realtime_payload(started_at: float) -> bytes:
    unix = time.time()
    utc_seconds = int(unix)
    utc_milliseconds = int((unix - utc_seconds) * 1000)
    monotonic_ms = int((time.monotonic() - started_at) * 1000) & 0xFFFFFFFF
    return struct.pack(
        "<iiiIIH8B",
        399042000, 1164074000, 120000,
        utc_seconds, monotonic_ms, utc_milliseconds,
        0x03, 3, 50, 2, 0, 85, 0x1F, 0x0F,
    )


def describe_status(payload: bytes) -> tuple[str, bool]:
    if len(payload) != 14:
        return f"invalid status length={len(payload)}", False
    state, capture, error, free, frames, session, safe, reserved = struct.unpack("<BBBBIIBB", payload)
    problems = []
    if state > 2:
        problems.append("bad-state")
    if capture > 1 or safe > 1 or free > 100 or reserved != 0:
        problems.append("bad-field")
    suffix = f" WARNING={','.join(problems)}" if problems else ""
    return (f"state={state} capture={capture} error={error} free={free}% "
            f"frames={frames} session={session} safe={safe}{suffix}"), not problems


@dataclass
class PendingCommand:
    command: int
    sequence: int
    payload: bytes
    attempts: int = 0
    sent_at: float = 0.0


class Emulator:
    def __init__(self, serial_port, args: argparse.Namespace) -> None:
        self.serial = serial_port
        self.args = args
        self.parser = Parser()
        self.sequence = 0
        self.linked = False
        self.started_at = time.monotonic()
        self.linked_at: float | None = None
        self.next_handshake = self.started_at
        self.next_realtime = self.started_at
        self.handshake_sequence = self.next_sequence()
        self.last_heartbeat: float | None = None
        self.last_heartbeat_sequence: int | None = None
        self.heartbeat_count = 0
        self.failures: list[str] = []
        self.acked_commands: set[int] = set()
        self.expected_phase = "idle"
        # These flags prove each phase was observed at least once, not that
        # every later report remained consistent. TODO: reject regressions
        # after confirmation and validate nonzero B-board error reports.
        self.observed_capture = False
        self.observed_stopped = False
        self.observed_safe = False
        self.last_capture_frame_count: int | None = None
        self.heartbeat_timeout_reported = False
        self.ignored_ack_commands: set[int] = set()
        self.pending: PendingCommand | None = None
        self.start_sent = False
        self.repeat_start_sent = False
        self.stop_sent = False
        self.power_sent = False
        self.bad_crc_sent = False
        self.session_id = args.session_id

    def next_sequence(self) -> int:
        value = self.sequence
        self.sequence = (self.sequence + 1) & 0xFF
        return value

    def fail(self, message: str) -> None:
        self.failures.append(message)
        print(f"{stamp()}  ERROR {message}")

    def transmit(self, command: int, sequence: int, payload: bytes, label: str,
                 corrupt_crc: bool = False) -> None:
        wire = bytearray(encode_frame(command, sequence, payload))
        if corrupt_crc:
            wire[-1] ^= 1
        self.serial.write(wire)
        print(f"{stamp()}  TX {label} seq={sequence}{' BAD_CRC' if corrupt_crc else ''}")

    def queue_command(self, command: int, payload: bytes, label: str) -> None:
        if self.pending is not None:
            return
        self.pending = PendingCommand(command, self.next_sequence(), payload)
        self.retry_pending(label)

    def retry_pending(self, label: str = "command") -> None:
        # Reuse both SEQ and payload; allocating a new SEQ would not test
        # frame-level duplicate handling at the B endpoint.
        assert self.pending is not None
        self.pending.attempts += 1
        self.pending.sent_at = time.monotonic()
        self.transmit(self.pending.command, self.pending.sequence,
                      self.pending.payload, f"{label} attempt={self.pending.attempts}")

    def handle_frame(self, frame: Frame, now: float) -> None:
        if frame.command == CMD_HANDSHAKE_RESPONSE:
            if len(frame.payload) != 5:
                self.fail(f"invalid handshake response length={len(frame.payload)}")
                return
            version, ready, firmware, request_sequence = struct.unpack("<BBHB", frame.payload)
            print(f"{stamp()}  RX handshake-ack seq={frame.sequence} version={version} "
                  f"ready={ready} fw=0x{firmware:04X} request_seq={request_sequence}")
            sequence_ok = (frame.sequence == self.handshake_sequence and
                           request_sequence == self.handshake_sequence)
            if not sequence_ok:
                self.fail(f"handshake response did not echo seq={self.handshake_sequence}")
            newly_linked = version == 1 and ready == 1 and sequence_ok
            if newly_linked and not self.linked:
                self.linked_at = now
                self.next_realtime = now
            self.linked = newly_linked
            return
        if frame.command == CMD_STATUS:
            if self.last_heartbeat_sequence is not None:
                expected_sequence = (self.last_heartbeat_sequence + 1) & 0xFF
                if frame.sequence != expected_sequence:
                    self.fail(f"heartbeat sequence expected={expected_sequence} got={frame.sequence}")
            self.last_heartbeat_sequence = frame.sequence
            interval = "first"
            if self.last_heartbeat is not None:
                elapsed = now - self.last_heartbeat
                interval = f"dt={elapsed:.3f}s"
                if not 0.8 <= elapsed <= 1.2:
                    interval += " TIMING_WARNING"
                    self.fail(f"heartbeat timing outside tolerance: {elapsed:.3f}s")
            self.last_heartbeat = now
            self.heartbeat_count += 1
            description, valid = describe_status(frame.payload)
            if not valid:
                self.fail("invalid heartbeat fields")
            if len(frame.payload) == 14:
                _, capture, _, _, frame_count, session, safe, _ = struct.unpack(
                    "<BBBBIIBB", frame.payload)
                if self.expected_phase == "capturing" and capture == 1 and session == self.session_id:
                    self.observed_capture = True
                    if (self.last_capture_frame_count is not None and
                            frame_count < self.last_capture_frame_count):
                        self.fail("capture frame count moved backwards after a retransmission")
                    self.last_capture_frame_count = frame_count
                elif self.expected_phase == "stopped" and capture == 0 and session == 0:
                    self.observed_stopped = True
                elif self.expected_phase == "safe" and capture == 0 and session == 0 and safe == 1:
                    self.observed_safe = True
            print(f"{stamp()}  RX heartbeat seq={frame.sequence} {interval} "
                  f"{description}")
            return
        if frame.command == CMD_ACK:
            if len(frame.payload) != 3:
                self.fail(f"invalid ACK length={len(frame.payload)}")
                return
            ack_command, ack_sequence, result = struct.unpack("<BBB", frame.payload)
            print(f"{stamp()}  RX ACK cmd=0x{ack_command:02X} seq={ack_sequence} result={result}")
            if (self.pending is not None and ack_command == self.pending.command and
                    ack_sequence == self.pending.sequence):
                if (self.args.simulate_lost_ack and result == 0 and
                        ack_command not in self.ignored_ack_commands):
                    # Leave pending intact, including its original send time,
                    # so the ordinary timeout path generates the retransmit.
                    self.ignored_ack_commands.add(ack_command)
                    print(f"{stamp()}  SIMULATE lost ACK for cmd=0x{ack_command:02X}; "
                          "waiting for same-SEQ retry")
                    return
                if result != 0:
                    self.fail(f"command 0x{ack_command:02X} returned result={result}")
                else:
                    self.acked_commands.add(ack_command)
                    if ack_command == CMD_START:
                        self.expected_phase = "capturing"
                    elif ack_command == CMD_STOP:
                        self.expected_phase = "stopped"
                    elif ack_command == CMD_POWER_OFF:
                        self.expected_phase = "safe"
                self.pending = None
            else:
                self.fail(f"unexpected ACK cmd=0x{ack_command:02X} seq={ack_sequence}")
            return
        self.fail(f"unexpected cmd=0x{frame.command:02X} seq={frame.sequence} len={len(frame.payload)}")

    def run(self) -> bool:
        deadline = self.started_at + self.args.duration
        while time.monotonic() < deadline:
            now = time.monotonic()
            incoming = self.serial.read(self.serial.in_waiting or 1)
            for frame in self.parser.feed(incoming, now):
                self.handle_frame(frame, now)

            if not self.linked and now >= self.next_handshake:
                self.transmit(CMD_HANDSHAKE, self.handshake_sequence,
                              handshake_payload(self.args.drone_sn), "handshake")
                self.next_handshake += 1.0
            elif self.linked and now >= self.next_realtime:
                corrupt = self.args.inject_bad_crc and not self.bad_crc_sent
                self.transmit(CMD_REALTIME, self.next_sequence(),
                              realtime_payload(self.started_at), "realtime", corrupt)
                self.bad_crc_sent |= corrupt
                self.next_realtime += 0.2

            elapsed = now - self.linked_at if self.linked_at is not None else 0.0
            if self.args.scenario == "mission" and self.linked and self.pending is None:
                if elapsed >= 3 and not self.start_sent:
                    self.start_sent = True
                    self.queue_command(CMD_START, struct.pack("<BBI", 1, 0, self.session_id), "start")
                elif elapsed >= 6 and not self.repeat_start_sent:
                    # Unlike a retry, this intentionally gets a new SEQ and
                    # tests session-level idempotency with the same session ID.
                    self.repeat_start_sent = True
                    self.queue_command(CMD_START, struct.pack("<BBI", 1, 0, self.session_id),
                                       "repeat-start")
                elif elapsed >= 12 and not self.stop_sent:
                    self.stop_sent = True
                    self.queue_command(CMD_STOP, struct.pack("<BBI", 1, 0, self.session_id), "stop")
                elif elapsed >= 15 and not self.power_sent:
                    self.power_sent = True
                    self.queue_command(CMD_POWER_OFF, struct.pack("<B", 10), "power-off")

            if self.pending is not None and now - self.pending.sent_at >= 0.2:
                # Protocol section 5.3 allows three retransmissions after the
                # initial transmission, matching the full-flight emulator.
                if self.pending.attempts >= ACTION_MAX_ATTEMPTS:
                    self.fail(f"no ACK after {ACTION_MAX_ATTEMPTS} attempts")
                    self.pending = None
                else:
                    self.retry_pending()

            heartbeat_reference = self.last_heartbeat if self.last_heartbeat is not None else self.linked_at
            if (self.linked and heartbeat_reference is not None and
                    now - heartbeat_reference > 3.0 and not self.heartbeat_timeout_reported):
                self.fail("heartbeat timeout (>3s)")
                self.heartbeat_timeout_reported = True
            time.sleep(0.005)

        if not self.linked:
            self.fail("handshake never completed")
        if self.heartbeat_count < 2:
            self.fail(f"only {self.heartbeat_count} heartbeat frame(s) received")
        if self.pending is not None:
            self.fail("test ended with an unacknowledged command")
        if self.args.scenario == "mission":
            missing = {CMD_START, CMD_STOP, CMD_POWER_OFF} - self.acked_commands
            if missing:
                self.fail("missing successful ACK(s): " +
                          ", ".join(f"0x{command:02X}" for command in sorted(missing)))
            if not self.observed_capture:
                self.fail("no heartbeat confirmed the capturing state")
            if not self.observed_stopped:
                self.fail("no heartbeat confirmed the stopped state with session=0")
            if not self.observed_safe:
                self.fail("no heartbeat confirmed safe power-off")
        if self.failures:
            print(f"{stamp()}  TEST FAILED ({len(self.failures)} failure(s))")
            return False
        print(f"{stamp()}  TEST PASSED ({self.heartbeat_count} heartbeats)")
        return True


def self_test() -> None:
    assert crc16_ccitt_false(b"123456789") == 0x29B1
    assert encode_frame(CMD_POWER_OFF, 7, b"\x0A") == bytes.fromhex("AA550130070A0C0F")
    parser = Parser()
    wire = encode_frame(CMD_STATUS, 9, struct.pack("<BBBBIIBB", 1, 0, 0, 80, 5, 42, 0, 0))
    frames = []
    for index, byte in enumerate(wire):
        frames.extend(parser.feed(bytes((byte,)), index / 1000.0))
    assert frames == [Frame(CMD_STATUS, 9, wire[5:-2])]
    expected = Frame(CMD_STATUS, 9, wire[5:-2])
    assert Parser().feed(b"\x00\x55\xAA\xAA\x42" * 100 + wire, 0) == [expected]
    parser = Parser()
    assert parser.feed(wire[:6], 0) == []
    assert parser.feed(b"\x12\xAA", 0.2) == []
    assert parser.feed(wire[1:], 0.201) == [expected]
    corrupted = bytearray(wire)
    corrupted[5] ^= 1
    assert Parser().feed(bytes(corrupted) + wire, 0) == [expected]
    assert Parser().feed(wire[:-1] + wire, 0) == [expected]
    assert Parser().feed(wire[:-2] + wire, 0) == [expected]
    assert Parser().feed(wire + wire, 0) == [expected, expected]
    print("A-board emulator self-test passed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB-UART port connected to B, for example COM14")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--scenario", choices=("heartbeat", "mission"), default="heartbeat")
    parser.add_argument("--session-id", type=lambda value: int(value, 0), default=0x00010001)
    parser.add_argument("--drone-sn", default="DJI-H1-A-EMULATOR")
    parser.add_argument("--inject-bad-crc", action="store_true")
    parser.add_argument("--simulate-lost-ack", action="store_true",
                        help="discard first successful ACK per action to exercise same-SEQ retries")
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.port:
        print("--port is required unless --self-test is used", file=sys.stderr)
        return 2
    try:
        import serial
    except ImportError:
        print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
        return 2
    with serial.Serial(args.port, args.baud, timeout=0.01) as serial_port:
        return 0 if Emulator(serial_port, args).run() else 1


if __name__ == "__main__":
    raise SystemExit(main())
