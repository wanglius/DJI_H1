"""Host-only capture instrumentation tests; no serial port is opened."""
import io
import threading
import time
import unittest

from debug_capture import DiagnosticCapture


class FakeSerial:
    dtr = False
    rts = False
    in_waiting = 0

    def __init__(self, blocked=False):
        self.first = True
        self.resumed = False
        self.blocked = blocked
        self.cancelled = threading.Event()
        self.opens = 0
        self.closed = False

    def read(self, size):
        if self.first:
            self.first = False
            return b'boot\n'
        if self.resumed:
            self.resumed = False
            return b'recovered\n'
        if self.blocked and not self.opens:
            self.cancelled.wait(2)
        else:
            time.sleep(.005)
        return b''

    def close(self):
        self.closed = True

    def open(self):
        assert self.closed
        self.opens += 1
        self.closed = False
        self.resumed = True

    def cancel_read(self):
        self.cancelled.set()


class CaptureTests(unittest.TestCase):
    def run_capture(self, port, reopen):
        chunks, errors = [], []
        capture = DiagnosticCapture(port, io.StringIO(), io.StringIO(), chunks, errors,
                                    silence=.08, interval=.01, allow_reopen=reopen)
        capture.start()
        try:
            deadline = time.monotonic()+1
            while time.monotonic() < deadline:
                if (reopen and 'recovered\n' in chunks) or (not reopen and len(chunks)):
                    break
                time.sleep(.01)
        finally:
            result = capture.finish()
        self.assertEqual(errors, [])
        self.assertFalse(result['reader_alive'])
        return result, chunks

    def test_read_only_default(self):
        port = FakeSerial()
        result, chunks = self.run_capture(port, False)
        self.assertEqual(chunks, ['boot\n'])
        self.assertEqual(result['bytes'], 5)
        self.assertEqual(port.opens, 0)

    def test_empty_reads_trigger_one_reopen(self):
        port = FakeSerial()
        result, chunks = self.run_capture(port, True)
        self.assertIn('recovered\n', chunks)
        self.assertGreater(result['empty_reads'], 0)
        self.assertEqual(result['reopen_attempts'], 1)
        self.assertEqual(port.opens, 1)
        self.assertFalse(port.dtr)
        self.assertFalse(port.rts)

    def test_blocked_read_cancelled_before_reopen(self):
        port = FakeSerial(blocked=True)
        result, chunks = self.run_capture(port, True)
        self.assertIn('recovered\n', chunks)
        self.assertTrue(port.cancelled.is_set())
        self.assertIn('blocked_read_cancelled', [e['event'] for e in result['events']])

    def test_silent_from_open_also_reopens(self):
        port = FakeSerial()
        port.first = False
        result, chunks = self.run_capture(port, True)
        self.assertEqual(chunks, ['recovered\n'])
        self.assertEqual(result['reopen_attempts'], 1)

    def test_read_error_reported(self):
        port = FakeSerial()
        def fail(_size):
            raise OSError('test read failure')
        port.read = fail
        errors = []
        capture = DiagnosticCapture(port, io.StringIO(), io.StringIO(), [], errors)
        capture.start()
        capture.reader.join(1)
        result = capture.finish()
        self.assertEqual(result['phase'], 'error')
        self.assertIn('test read failure', errors[0])


if __name__ == '__main__':
    unittest.main()
