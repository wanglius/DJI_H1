"""Bounded command edge cases, used by run_hardware_flight.py --probe.

This is an A-board client, not a fake B implementation. It sends navigation
continuously and rejects heartbeat/ACK delays while exercising real hardware.
"""
import json
import struct
import time
from ab_board_emulator import (Parser, encode_frame, realtime_payload,
    CMD_HANDSHAKE, CMD_HANDSHAKE_RESPONSE, CMD_REALTIME, CMD_START, CMD_STOP,
    CMD_POWER_OFF, CMD_STATUS, CMD_ACK)


class ControlProbe:
    def __init__(self, uart, args):
        self.uart, self.args = uart, args
        self.parser = Parser()
        self.boot = time.monotonic()
        self.sequence = 0
        self.linked = False
        self.next_nav = self.boot
        self.acks = {}
        self.ready_sequences = set()
        self.last_hb = None
        self.last_seq = None
        self.status = None
        self.events = []
        self.final_counts = {}

    def log(self, event, **fields):
        row = dict(t=round(time.monotonic() - self.boot, 3), event=event, **fields)
        self.events.append(row)
        print(json.dumps(row), flush=True)

    def seq(self):
        seq = self.sequence
        self.sequence = (seq + 1) & 255
        return seq

    def send(self, cmd, seq, payload):
        wire = encode_frame(cmd, seq, payload)
        if self.uart.write(wire) != len(wire):
            raise RuntimeError('short UART write')

    def pump(self):
        now = time.monotonic()
        if self.linked and now >= self.next_nav:
            self.send(CMD_REALTIME, self.seq(), realtime_payload(self.boot))
            self.next_nav = now + .2
        raw = self.uart.read(self.uart.in_waiting or 1)
        for frame in self.parser.feed(raw, time.monotonic()):
            now = time.monotonic()
            if frame.command == CMD_HANDSHAKE_RESPONSE:
                version, ready, _, echo = struct.unpack('<BBHB', frame.payload)
                assert version == 1 and echo == frame.sequence
                if ready:
                    self.ready_sequences.add(echo)
            elif frame.command == CMD_ACK:
                cmd, seq, result = frame.payload
                assert seq == frame.sequence
                self.acks[cmd, seq] = result
            elif frame.command == CMD_STATUS:
                self.status = struct.unpack('<BBBBIIBB', frame.payload)
                state, capture, error, free, count, session, safe, reserved = self.status
                assert state in (0, 1) and error == 0, self.status
                assert capture in (0, 1) and safe in (0, 1) and free <= 100 and not reserved
                assert not (capture and safe)
                if self.last_hb is not None:
                    assert .75 <= now - self.last_hb <= 1.25, 'heartbeat cadence'
                    assert frame.sequence == (self.last_seq + 1) & 255, 'heartbeat sequence'
                self.last_hb, self.last_seq = now, frame.sequence
                self.log('heartbeat', capture=capture, frames=count, session=session,
                         safe=safe, free_percent=free)
        if self.last_hb is not None:
            assert time.monotonic() - self.last_hb < 3, 'heartbeat timeout'

    def wait(self, predicate, timeout, label):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump()
            if predicate():
                return
        raise RuntimeError('timeout: ' + label)

    def hold_idle(self, duration, count):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.pump()
            assert self.status[1] == 0 and self.status[5] == 0
            assert self.status[4] == count, 'idle frame count changed'

    def command(self, cmd, payload, expected=0, seq=None):
        seq = self.seq() if seq is None else seq
        key = (cmd, seq)
        self.acks.pop(key, None)
        for attempt in range(4):
            sent = time.monotonic()
            self.send(cmd, seq, payload)
            while time.monotonic() - sent < .2:
                self.pump()
                if key in self.acks:
                    assert self.acks[key] == expected, (cmd, self.acks[key], expected)
                    self.log('command-pass', cmd=cmd, seq=seq, result=expected, attempt=attempt+1)
                    return seq
        raise RuntimeError('ACK timeout')

    @staticmethod
    def payload(session, reason=1):
        return struct.pack('<BBI', reason, 0, session)

    def run(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not self.linked:
            seq = self.seq()
            self.send(CMD_HANDSHAKE, seq, struct.pack('<BBH32s', 1, 1, 1, b'CONTROL-PROBE'))
            retry = time.monotonic() + 1
            while time.monotonic() < retry:
                self.pump()
                if seq in self.ready_sequences:
                    self.linked = True
                    break
        assert self.linked, 'B not ready'
        self.wait(lambda: self.status is not None, 3, 'first heartbeat')
        self.hold_idle(2, 0)

        # Send START/STOP in one UART burst to hit cancellation-before-preparation.
        a, b = self.seq(), self.seq()
        wire = encode_frame(CMD_START, a, self.payload(0)) + encode_frame(CMD_STOP, b, self.payload(0))
        assert self.uart.write(wire) == len(wire)
        self.wait(lambda: (CMD_START, a) in self.acks and (CMD_STOP, b) in self.acks,
                  .2, 'immediate start/stop ACKs')
        assert self.acks[CMD_START, a] == self.acks[CMD_STOP, b] == 0
        self.hold_idle(3, 0)
        self.command(CMD_START, self.payload(0))
        self.hold_idle(2, 0)
        self.log('cancelled-session-zero-replay-pass')

        first, second = self.args.session_id, self.args.session_id + 1
        start_seq = self.command(CMD_START, self.payload(first))
        self.wait(lambda: self.status[1] == 1 and self.status[4] >= 4 and self.status[5] == first,
                  10, 'first real acquisition')
        before = self.status[4]
        self.command(CMD_START, self.payload(first), seq=start_seq)
        self.command(CMD_START, self.payload(first))
        self.command(CMD_START, self.payload(second), expected=2)
        self.command(CMD_STOP, self.payload(second), expected=4)
        self.command(CMD_STOP, self.payload(first, reason=0), expected=3)
        self.wait(lambda: self.status[4] > before and self.status[5] == first,
                  4, 'active session continued without reset')
        self.command(CMD_STOP, self.payload(first))
        self.wait(lambda: self.status[1] == 0 and self.status[5] == 0, 10, 'first stop')
        self.final_counts[first] = self.status[4]
        self.command(CMD_START, self.payload(first))
        self.hold_idle(2, self.status[4])

        self.command(CMD_START, self.payload(second))
        self.wait(lambda: self.status[1] == 1 and self.status[4] >= 4 and self.status[5] == second,
                  10, 'restarted real acquisition')
        before = self.status[4]
        # Old completed-session commands must not stop/restart the new session.
        self.command(CMD_START, self.payload(first))
        self.command(CMD_STOP, self.payload(first))
        self.wait(lambda: self.status[4] > before and self.status[5] == second,
                  4, 'new session survived stale commands')
        self.command(CMD_POWER_OFF, bytes((10,)))
        self.command(CMD_START, self.payload(second + 1), expected=4)
        self.wait(lambda: self.status[6] == 1 and self.status[1] == 0 and self.status[5] == 0,
                  10, 'power-off during active acquisition')
        self.final_counts[second] = self.status[4]
        self.hold_idle(2, self.status[4])
        self.log('CONTROL-PROBE-PASSED')
        return dict(passed=True, events=self.events, failures=[],
                    session_final_counts=self.final_counts)
