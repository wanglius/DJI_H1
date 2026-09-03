"""Complete, bounded A-board flight mission over a 3.3V UART (default COM5).

Protocol codec is shared with the milestone emulator; this runner owns the
mission state and transport. No physical power control or B-board reset.
"""
import argparse
from collections import deque
from dataclasses import dataclass
import json
import secrets
import struct
import time

from ab_board_emulator import (
    Parser, encode_frame, CMD_HANDSHAKE, CMD_HANDSHAKE_RESPONSE, CMD_REALTIME,
    CMD_START, CMD_STOP, CMD_POWER_OFF, CMD_ACK, CMD_STATUS,
)
from flight_model import FlightModel, STAGES


@dataclass
class Request:
    command: int
    sequence: int
    payload: bytes
    label: str
    sent: float = 0
    attempts: int = 0


class FlightEmulator:
    def __init__(self, uart, args, clock=time.monotonic, wall_clock=time.time):
        self.uart, self.args, self.clock, self.wall_clock = uart, args, clock, wall_clock
        self.model = FlightModel(args.duration, args.scenario)
        self.parser = Parser()
        self.boot = clock()
        self.mission_start = None
        self.sequence = 0
        self.handshake = None
        self.linked = False
        self.next_handshake = self.boot
        self.next_nav = self.boot
        self.tx_resume = self.boot
        self.last_heartbeat = None
        self.last_hb_sequence = None
        self.hb_count = 0
        self.nav_count = 0
        self.pending = None
        self.queue = deque()
        self.ack_history = deque(maxlen=32)
        self.dropped = set()
        self.completed = set()
        self.stages = set()
        self.failures = []
        self.events = []
        self.desired_capture = False
        self.desired_session = args.session_id
        self.planned_session = args.session_id
        task = ((args.session_id & 0xFFFF) + 1) & 0xFFFF
        self.restart_session = (args.session_id & 0xFFFF0000) | (task or 1)
        self.capture_started = False
        self.confirmed_capture = False
        self.session_frames = {}
        self.session_final_counts = {}
        self.confirmed_stop = False
        self.frame_count = None
        self.state_changed = self.boot
        self.last_sync = self.boot
        self.power_sent = None
        self.safe = False
        self.finished = False
        self.reconnections = 0
        self.supplement_sent = False
        self.crc_sent = False
        self.truncated_sent = False
        self.blackout_seen = False
        self.blackout_active = False

    def log(self, event, **fields):
        row = dict(t=round(self.clock() - self.boot, 3), event=event, **fields)
        self.events.append(row)
        print(json.dumps(row), flush=True)

    def fail(self, reason):
        if reason not in self.failures:
            self.failures.append(reason)
            self.log("FAIL", reason=reason)

    def seq(self):
        value = self.sequence
        self.sequence = (value + 1) & 255
        return value

    def send(self, command, sequence, payload):
        wire = encode_frame(command, sequence, payload)
        if self.uart.write(wire) != len(wire):
            raise OSError("short UART write")

    def begin_handshake(self, connected):
        serial = self.args.drone_sn.encode("ascii") if connected else b""
        self.handshake = Request(CMD_HANDSHAKE, self.seq(),
            struct.pack("<BBH32s", 1, int(connected), 0x100, serial), "handshake")
        self.next_handshake = self.clock()

    def enqueue(self, command, label, reason=1, session=None):
        # Stage processing may enqueue several commands before transmission.
        # Bind their session now, independently of the last transmitted target.
        session = self.planned_session if session is None else session
        payload = (bytes((self.args.grace,)) if command == CMD_POWER_OFF else
                   struct.pack("<BBI", 1 if command == CMD_START else reason,
                               0, session))
        self.queue.append((command, payload, label))

    def sync_capture(self):
        # A new sequence synchronizes target state after reconnect (doc 5.5).
        if self.capture_started:
            self.enqueue(CMD_START if self.desired_capture else CMD_STOP, "state-sync",
                         session=self.desired_session)
        self.last_sync = self.clock()

    def receive(self, frame, now):
        if frame.command == CMD_HANDSHAKE_RESPONSE:
            if len(frame.payload) != 5:
                self.fail("malformed handshake response")
                return
            version, ready, _, echo = struct.unpack("<BBHB", frame.payload)
            if (not self.handshake or frame.sequence != self.handshake.sequence or
                    echo != self.handshake.sequence or version != 1 or ready not in (0, 1)):
                self.fail("invalid handshake version/sequence/readiness")
                return
            if not ready:
                return  # Initialization is not a failed handshake; keep 1Hz retries.
            was_linked = self.linked
            reconnect = not was_linked and self.mission_start is not None
            self.linked = True
            self.handshake = None
            if self.mission_start is None:
                self.mission_start = now
                self.state_changed = now
            if reconnect:
                self.reconnections += 1
                self.sync_capture()
            if not was_linked:
                self.last_heartbeat = now  # Allow up to 3s for the first report.
                self.last_hb_sequence = None
            self.next_nav = now
            self.log("linked", reconnect=reconnect)
        elif frame.command == CMD_ACK:
            if len(frame.payload) != 3:
                self.fail("malformed ACK")
                return
            cmd, seq, result = frame.payload
            key = (cmd, seq)
            if frame.sequence != seq:
                self.fail("ACK header sequence differs from echoed sequence")
            if not self.pending or key != (self.pending.command, self.pending.sequence):
                if key not in self.ack_history:
                    self.fail("unmatched ACK")
                return
            if self.args.lost_ack and key not in self.dropped and result == 0:
                self.dropped.add(key)
                self.log("inject-lost-ack", command=cmd, sequence=seq)
                return
            self.log("ack", command=cmd, sequence=seq, result=result, label=self.pending.label)
            if result:
                self.fail(f"negative ACK {result} for {self.pending.label}")
            else:
                self.completed.add(self.pending.label)
            self.ack_history.append(key)
            self.pending = None
        elif frame.command == CMD_STATUS:
            if not self.linked:
                return  # Reconnection is established by 0xA0, not a stray status.
            if len(frame.payload) != 14:
                self.fail("malformed heartbeat")
                return
            state, capture, error, free, count, session, safe, reserved = struct.unpack(
                "<BBBBIIBB", frame.payload)
            if state > 2 or capture > 1 or free > 100 or safe > 1 or reserved:
                self.fail("invalid heartbeat fields")
            if error or state == 2:
                self.fail(f"B-board fault state={state} error={error}")
            if self.last_hb_sequence is not None:
                if frame.sequence != ((self.last_hb_sequence + 1) & 255):
                    self.fail("heartbeat sequence gap outside injected outage")
                interval = now - self.last_heartbeat
                if not .75 <= interval <= 1.25:
                    self.fail("heartbeat cadence outside 0.75–1.25 seconds")
            self.last_hb_sequence = frame.sequence
            self.last_heartbeat = now
            self.hb_count += 1
            if capture and safe:
                self.fail("B reports capturing and safe-power-off simultaneously")
            matches = bool(capture) == self.desired_capture and session == (
                self.desired_session if self.desired_capture else 0)
            if matches and self.desired_capture:
                self.confirmed_capture = True
                if self.frame_count is not None and count < self.frame_count:
                    self.fail("frame counter reset within session")
                self.frame_count = count
                self.session_frames[session] = count
            elif matches and self.capture_started:
                self.confirmed_stop = True
                previous = self.session_frames.get(self.desired_session, 0)
                final = self.session_final_counts.get(self.desired_session)
                if count < previous or (final is not None and count != final):
                    self.fail("frame counter changed or regressed after stopping")
                self.session_final_counts[self.desired_session] = count
            # Check every report, not just whether a phase was once observed.
            if not matches and now - self.state_changed > 3:
                self.fail("capture/session state disagrees with target for over 3s")
                if now - self.last_sync >= 3 and not self.pending and not self.queue:
                    self.sync_capture()
            self.log("heartbeat", state=state, error=error, free_percent=free,
                     capture=capture, frames=count, session=session, safe=safe)
            if self.power_sent is not None and safe and not capture and session == 0:
                self.safe = True
                if "power-off" in self.completed:
                    self.log("simulated-power-cut", cause="safe-power-off")
                    self.finished = True
        else:
            self.fail(f"unexpected command {frame.command:#x}")

    def step(self):
        now = self.clock()
        elapsed = 0 if self.mission_start is None else now - self.mission_start
        outage_start = self.args.duration * .30 + 4
        blackout = self.args.blackout and outage_start <= elapsed < outage_start + 4.5
        if blackout != self.blackout_active:
            self.blackout_active = blackout
            self.blackout_seen |= blackout
            self.log("injected-link-outage" if blackout else "injected-link-restored")
            self.parser = Parser()
            self.last_hb_sequence = None
        incoming = self.uart.read(self.uart.in_waiting or 1)
        if not blackout:
            for frame in self.parser.feed(incoming, self.clock()):
                self.receive(frame, self.clock())
        if self.finished:
            return
        now = self.clock()
        elapsed = 0 if self.mission_start is None else now - self.mission_start
        if now < self.tx_resume:
            return  # Preserve an intentional >100ms inter-byte gap after truncation.
        if self.linked and self.last_heartbeat is not None and now - self.last_heartbeat > 3:
            if not blackout:
                self.fail("unexpected heartbeat timeout")
            self.log("reconnect-start")
            self.linked = False
            self.last_hb_sequence = None
            # Drop stale transport work; desired state is synchronized once linked.
            if self.pending:
                self.fail("link lost with an outstanding action")
            self.pending = None
            self.begin_handshake(True)
        if blackout:
            return
        if not self.linked and self.handshake is None:
            self.begin_handshake(self.mission_start is not None)
        if self.handshake and now >= self.next_handshake:
            self.send(CMD_HANDSHAKE, self.handshake.sequence, self.handshake.payload)
            self.next_handshake = now + 1
            self.log("handshake-tx", sequence=self.handshake.sequence)
        if not self.linked:
            return
        # Stage events execute once in order, even if a scheduling pause skips one.
        for stage in STAGES:
            if elapsed >= stage.fraction * self.args.duration and stage.name not in self.stages:
                self.stages.add(stage.name)
                self.log("stage", name=stage.name)
                if stage.name == "preflight":
                    self.begin_handshake(True)
                    self.supplement_sent = True
                elif stage.name == "survey":
                    self.enqueue(CMD_START, "survey-start")
                elif stage.name == "restart-survey":
                    self.planned_session = self.restart_session
                    self.enqueue(CMD_START, "restart-start", session=self.restart_session)
                elif stage.name == "power-off":
                    self.enqueue(CMD_POWER_OFF, "power-off")
                elif stage.stop_reason:
                    reason = self.model.stage(stage.fraction * self.args.duration).stop_reason
                    self.enqueue(CMD_STOP, stage.name, reason)
        if now >= self.next_nav:
            payload = self.model.telemetry(elapsed, self.wall_clock(), now - self.boot)
            wire = encode_frame(CMD_REALTIME, self.seq(), payload)
            if self.args.bad_frames and elapsed >= self.args.duration * .35 and not self.crc_sent:
                wire = wire[:-1] + bytes((wire[-1] ^ 1,))
                self.crc_sent = True
                self.log("inject-bad-crc")
            elif self.args.bad_frames and elapsed >= self.args.duration * .40 and not self.truncated_sent:
                wire = wire[:8]
                self.truncated_sent = True
                self.tx_resume = now + .15
                self.log("inject-truncated-frame")
            if self.uart.write(wire) != len(wire):
                raise OSError("short telemetry write")
            self.nav_count += 1
            # Keep the 5Hz grid instead of accumulating serial-read/scheduler
            # latency; skip missed slots rather than bursting stale telemetry.
            self.next_nav += (int(max(0, now - self.next_nav) / .2) + 1) * .2
            if now < self.tx_resume:
                return
        if self.pending is None and self.queue:
            cmd, payload, label = self.queue.popleft()
            self.pending = Request(cmd, self.seq(), payload, label)
            if cmd in (CMD_START, CMD_STOP):
                desired = cmd == CMD_START
                requested_session = struct.unpack_from("<I", payload, 2)[0]
                if desired != self.desired_capture or (desired and
                        requested_session != self.desired_session):
                    self.state_changed = now
                    self.frame_count = None
                self.desired_capture = desired
                if desired:
                    self.desired_session = requested_session
                self.capture_started |= desired
            else:
                self.power_sent = now
        if self.pending and (not self.pending.attempts or now - self.pending.sent >= .2):
            # Doc says at most three retransmissions: initial + three retries.
            if self.pending.attempts >= 4:
                self.fail(f"ACK timeout: {self.pending.label}")
                self.pending = None
            else:
                self.send(self.pending.command, self.pending.sequence, self.pending.payload)
                self.pending.attempts += 1
                self.pending.sent = self.clock()
                self.log("action-tx", label=self.pending.label, sequence=self.pending.sequence,
                         attempt=self.pending.attempts)
        if self.power_sent is not None and now - self.power_sent >= self.args.grace:
            self.fail("power-off grace expired without acknowledged safe shutdown")
            self.log("simulated-power-cut", cause="grace-expired")
            self.finished = True

    def report(self):
        required = {"survey-start", "survey-complete", "restart-start", "return-home", "landing",
                    "landed", "prepare-shutdown", "power-off"}
        for label in sorted(required - self.completed):
            self.fail(f"missing successful action: {label}")
        if not self.confirmed_capture or not self.confirmed_stop or not self.safe:
            self.fail("missing capture/stop/safe heartbeat confirmation")
        for session in (self.args.session_id, self.restart_session):
            if self.session_frames.get(session, 0) == 0:
                self.fail(f"no acquired frames observed for session {session}")
            if session not in self.session_final_counts:
                self.fail(f"no stopped heartbeat observed for session {session}")
        if self.handshake is not None:
            self.fail("handshake still outstanding")
        if self.args.blackout and (not self.blackout_seen or not self.reconnections):
            self.fail("requested reconnect fault was not exercised")
        if self.args.bad_frames and not (self.crc_sent and self.truncated_sent):
            self.fail("requested bad-frame faults were not exercised")
        return dict(passed=not self.failures, session_id=self.args.session_id,
                    session_final_counts=self.session_final_counts,
                    scenario=self.args.scenario, heartbeats=self.hb_count,
                    navigation_frames=self.nav_count, reconnections=self.reconnections,
                    failures=self.failures, events=self.events,
                    scope="protocol and reported B state; no physical power removal")

    def run(self):
        deadline = self.boot + self.args.duration + self.args.grace + 20
        while not self.finished and self.clock() < deadline:
            self.step()
            time.sleep(.005)
        if not self.finished:
            self.fail("overall mission timeout")
        return self.report()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--duration", type=float, default=60)
    parser.add_argument("--grace", type=int, default=10)
    parser.add_argument("--scenario", choices=("normal", "low-battery", "manual-abort",
                                              "drone-link-loss"), default="normal")
    parser.add_argument("--session-id", type=lambda x: int(x, 0),
                        default=(secrets.randbelow(65535) + 1) << 16 | 1)
    parser.add_argument("--drone-sn", default="DJI-H1-SIMULATED-FLIGHT")
    parser.add_argument("--lost-ack", action="store_true")
    parser.add_argument("--blackout", action="store_true")
    parser.add_argument("--bad-frames", action="store_true")
    parser.add_argument("--report", help="new JSON report file; existing files are not overwritten")
    args = parser.parse_args()
    if not 60 <= args.duration <= 86400 or not 1 <= args.grace <= 255:
        parser.error("duration must be 60..86400 seconds; grace must be 1..255")
    if not 0 <= args.session_id <= 0xFFFFFFFF:
        parser.error("session ID must fit uint32")
    try:
        serial = args.drone_sn.encode("ascii")
    except UnicodeEncodeError:
        parser.error("drone serial must be ASCII")
    if len(serial) > 32:
        parser.error("drone serial must be at most 32 bytes")
    return args


def main():
    args = parse_args()
    import serial
    # Exclusively create output before starting, so a bad path fails before UART actions.
    from contextlib import ExitStack
    with ExitStack() as stack:
        output = stack.enter_context(open(args.report, "x", encoding="utf-8")) if args.report else None
        uart = stack.enter_context(serial.Serial(args.port, 115200, timeout=.01, write_timeout=1))
        result = FlightEmulator(uart, args).run()
        if output:
            json.dump(result, output, indent=2)
    print("FLIGHT TEST PASSED" if result["passed"] else "FLIGHT TEST FAILED", flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
