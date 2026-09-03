"""Flight-model and end-to-end runner tests using a virtual clock/UART."""
import argparse
import contextlib
import io
import struct
import unittest

from ab_board_emulator import Frame, Parser, encode_frame, CMD_HANDSHAKE, CMD_HANDSHAKE_RESPONSE
from ab_board_emulator import CMD_REALTIME, CMD_START, CMD_STOP, CMD_POWER_OFF, CMD_ACK, CMD_STATUS
from flight_model import FlightModel
from flight_emulator import FlightEmulator


class Clock:
    now = 0.0

    def __call__(self):
        return self.now


class FakeB:
    """Simulated endpoint only; this does not execute the ESP32 C firmware."""
    def __init__(self, clock):
        self.clock = clock
        self.parser = Parser()
        self.buffer = bytearray()
        self.linked = False
        self.capture = False
        self.safe = False
        self.session = 0
        self.count = 0
        self.seq = 0
        self.next_hb = 0
        self.received = []
        self.error = 0
        self.no_ack = False
        self.regress = False

    def emit(self, cmd, seq, payload):
        self.buffer.extend(encode_frame(cmd, seq, payload))

    def write(self, wire):
        for frame in self.parser.feed(wire, self.clock()):
            self.received.append(frame)
            if frame.command == CMD_HANDSHAKE:
                self.emit(CMD_HANDSHAKE_RESPONSE, frame.sequence,
                          struct.pack("<BBHB", 1, 1, 1, frame.sequence))
                if not self.linked:
                    self.next_hb = self.clock() + .5
                self.linked = True
            elif frame.command in (CMD_START, CMD_STOP, CMD_POWER_OFF):
                if frame.command == CMD_START:
                    session = struct.unpack_from("<I", frame.payload, 2)[0]
                    if not self.capture:
                        self.count = 0
                    self.session, self.capture, self.safe = session, True, False
                elif frame.command == CMD_STOP:
                    self.capture, self.session = False, 0
                else:
                    self.capture, self.session, self.safe = False, 0, True
                if not self.no_ack:
                    self.emit(CMD_ACK, frame.sequence, bytes((frame.command, frame.sequence, 0)))
        return len(wire)

    def tick(self):
        if self.linked and self.clock() >= self.next_hb:
            self.next_hb += 1
            self.count += int(self.capture)
            capture = self.capture and not (self.regress and self.count > 3)
            self.emit(CMD_STATUS, self.seq, struct.pack("<BBBBIIBB", 1, int(capture),
                      self.error, 75, self.count, self.session, int(self.safe), 0))
            self.seq = (self.seq + 1) & 255

    @property
    def in_waiting(self):
        return len(self.buffer)

    def read(self, count):
        data = bytes(self.buffer[:count])
        del self.buffer[:count]
        return data


class FlightTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.uart = FakeB(self.clock)
        self.args = argparse.Namespace(duration=60, scenario="normal", session_id=123,
            grace=10, drone_sn="SIM", lost_ack=False, blackout=False, bad_frames=False)
        self.quiet = contextlib.redirect_stdout(io.StringIO())
        self.quiet.__enter__()
        self.addCleanup(self.quiet.__exit__, None, None, None)

    def mission(self):
        runner = FlightEmulator(self.uart, self.args, self.clock, lambda: 1800000000 + self.clock())
        for _ in range(10000):
            self.uart.tick()
            runner.step()
            self.clock.now += .01
            if runner.finished:
                break
        self.assertTrue(runner.finished)
        return runner.report()

    def test_normal_complete_flight(self):
        result = self.mission()
        self.assertTrue(result["passed"], result["failures"])
        handshakes = [f for f in self.uart.received if f.command == CMD_HANDSHAKE]
        self.assertEqual(handshakes[0].payload[1], 0)
        self.assertEqual(handshakes[1].payload[1], 1)
        reasons = [f.payload[0] for f in self.uart.received if f.command == CMD_STOP]
        self.assertEqual(reasons, [1, 2, 4, 5, 8])
        self.assertGreater(result["navigation_frames"], 250)
        self.assertEqual(set(result["session_final_counts"]), {123, 124})

    def test_faults_and_reconnect(self):
        self.args.lost_ack = self.args.blackout = self.args.bad_frames = True
        result = self.mission()
        self.assertTrue(result["passed"], result["failures"])
        self.assertEqual(result["reconnections"], 1)
        starts = [f for f in self.uart.received if f.command == CMD_START]
        self.assertEqual(starts[0].sequence, starts[1].sequence)
        original_session = [f for f in starts if f.payload == starts[0].payload]
        self.assertGreaterEqual(len(original_session), 4)
        self.assertNotEqual(original_session[0].sequence, original_session[-1].sequence)
        self.assertNotEqual(starts[0].payload, starts[-1].payload)

    def test_error_report_fails(self):
        self.uart.error = 1
        self.assertFalse(self.mission()["passed"])

    def test_alternate_stop_reasons(self):
        for scenario, reason in (("low-battery", 3), ("manual-abort", 6), ("drone-link-loss", 7)):
            with self.subTest(scenario=scenario):
                self.clock = Clock()
                self.uart = FakeB(self.clock)
                self.args.scenario = scenario
                result = self.mission()
                self.assertTrue(result["passed"], result["failures"])
                reasons = [f.payload[0] for f in self.uart.received if f.command == CMD_STOP]
                self.assertIn(reason, reasons)

    def test_later_state_regression_fails(self):
        self.uart.regress = True
        self.assertFalse(self.mission()["passed"])

    def test_missing_ack_fails(self):
        self.uart.no_ack = True
        self.assertFalse(self.mission()["passed"])

    def test_mixed_rtk_position_gps_height(self):
        fields = struct.unpack("<iiiIIH8B", FlightModel().telemetry(25, 1800000000.5, 25))
        self.assertEqual(fields[6], 1)
        self.assertEqual(fields[8], 50)
        self.assertEqual(fields[12] & 16, 0)
        self.assertEqual(fields[13], 15)

    def test_uptime_rollover(self):
        fields = struct.unpack("<iiiIIH8B", FlightModel().telemetry(25, 1800000000, (2**32 + 1000)/1000))
        self.assertEqual(fields[4], 1000)

    def test_link_loss_clears_validity(self):
        model = FlightModel(scenario="drone-link-loss")
        fields = struct.unpack("<iiiIIH8B", model.telemetry(45, 1800000000, 45))
        self.assertEqual(fields[13], 0)
        self.assertEqual(fields[12], 0)
        self.assertEqual(model.stage(43).stop_reason, 7)

    def stopped_runner(self):
        runner = FlightEmulator(self.uart, self.args, self.clock)
        runner.linked = runner.capture_started = True
        runner.session_frames[123] = 5
        return runner

    def status(self, seq, count):
        return Frame(CMD_STATUS, seq, struct.pack('<BBBBIIBB', 1, 0, 0, 75, count, 0, 0, 0))

    def test_first_stopped_count_cannot_regress(self):
        runner = self.stopped_runner()
        runner.receive(self.status(0, 4), 1)
        self.assertIn('frame counter changed or regressed after stopping', runner.failures)

    def test_stopped_count_must_remain_constant(self):
        runner = self.stopped_runner()
        runner.receive(self.status(0, 6), 1)  # Last in-flight frame may complete.
        self.assertFalse(runner.failures)
        runner.receive(self.status(1, 7), 2)
        self.assertIn('frame counter changed or regressed after stopping', runner.failures)

    def test_delayed_stage_commands_bind_new_session(self):
        runner = FlightEmulator(self.uart, self.args, self.clock)
        runner.linked = True
        runner.mission_start = 0
        self.clock.now = 45  # Several stages became due before they could transmit.
        runner.step()
        stops = [(payload, label) for cmd, payload, label in runner.queue if cmd == CMD_STOP]
        sessions = {label: struct.unpack_from('<I', payload, 2)[0] for payload, label in stops}
        self.assertEqual(sessions['survey-complete'], 123)
        self.assertEqual(sessions['return-home'], 124)


if __name__ == "__main__":
    unittest.main()
