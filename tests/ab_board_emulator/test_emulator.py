"""Host-only regression tests; run with python -B -m unittest discover
-s tests/ab_board_emulator -p test_emulator.py. No UART is opened.
"""

import argparse
import contextlib
import io
import struct
import unittest

from ab_board_emulator import (
    CMD_ACK, CMD_HANDSHAKE_RESPONSE, CMD_POWER_OFF, CMD_START, CMD_STATUS,
    CMD_STOP, Emulator, Frame, PendingCommand, self_test,
)


class EmulatorTests(unittest.TestCase):
    def setUp(self):
        self.output = contextlib.redirect_stdout(io.StringIO())
        self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)
        self.args = argparse.Namespace(session_id=42, simulate_lost_ack=False,
                                       duration=0, scenario="mission")
        self.emulator = Emulator(None, self.args)

    def ack(self, command, sequence=7, result=0):
        self.emulator.handle_frame(
            Frame(CMD_ACK, sequence, bytes((command, sequence, result))), 0)

    def heartbeat(self, sequence, count, capture, session, safe=0):
        self.emulator.handle_frame(Frame(CMD_STATUS, sequence, struct.pack(
            "<BBBBIIBB", 1, capture, 0, 75, count, session, safe, 0)), sequence + 1.0)

    def test_parser_negative_paths(self):
        self_test()

    def test_no_handshake_cannot_pass(self):
        self.assertFalse(self.emulator.run())

    def test_bad_ack_fails(self):
        self.emulator.pending = PendingCommand(CMD_START, 7, b"")
        self.ack(CMD_START, result=3)
        self.assertTrue(self.emulator.failures)
        self.assertNotIn(CMD_START, self.emulator.acked_commands)

    def test_lost_ack_keeps_request_pending(self):
        self.args.simulate_lost_ack = True
        self.emulator.pending = PendingCommand(CMD_START, 7, b"")
        self.ack(CMD_START)
        self.assertIsNotNone(self.emulator.pending)
        self.ack(CMD_START)
        self.assertIsNone(self.emulator.pending)
        self.assertFalse(self.emulator.failures)

    def test_counter_reset_detected(self):
        self.emulator.expected_phase = "capturing"
        self.heartbeat(0, 8, 1, 42)
        self.heartbeat(1, 1, 1, 42)
        self.assertTrue(self.emulator.failures)

    def test_stale_session_does_not_confirm_stop(self):
        self.emulator.expected_phase = "stopped"
        self.heartbeat(0, 9, 0, 42)
        self.assertFalse(self.emulator.observed_stopped)
        self.heartbeat(1, 9, 0, 0)
        self.assertTrue(self.emulator.observed_stopped)

    def test_complete_simulated_mission_passes(self):
        self.emulator.handle_frame(Frame(CMD_HANDSHAKE_RESPONSE, 0,
            struct.pack("<BBHB", 1, 1, 1, 0)), 0)
        for index, command in enumerate((CMD_START, CMD_STOP, CMD_POWER_OFF)):
            self.emulator.pending = PendingCommand(command, 7, b"")
            self.ack(command)
            self.heartbeat(index, 1, int(command == CMD_START),
                           42 if command == CMD_START else 0,
                           int(command == CMD_POWER_OFF))
        self.assertTrue(self.emulator.run())


if __name__ == "__main__":
    unittest.main()
