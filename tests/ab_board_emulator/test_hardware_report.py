"""Host checks for the debug/heartbeat cross-check (does not open UARTs)."""
import unittest
from run_hardware_flight import verify_debug


class HardwareReportTests(unittest.TestCase):
    def setUp(self):
        self.log = ('H1-A frames OK : 4\nH1-B frames OK : 5\n'
                    'H1-A frames OK : 2\nH1-B frames OK : 3\n'
                    'Shutdown complete: safe=1 result=ESP_OK\n')
        self.report = {'session_final_counts': {11: 4, 12: 2}}

    def test_matching_counts(self):
        self.assertEqual(verify_debug(self.log, self.report), [])

    def test_bad_count_rejected(self):
        self.report['session_final_counts'][11] = 10
        self.assertTrue(verify_debug(self.log, self.report))

    def test_missing_channel_rejected(self):
        self.assertTrue(verify_debug(self.log.replace('H1-A frames OK : 2', ''), self.report))

    def test_overruns_rejected(self):
        self.assertTrue(verify_debug(self.log + 'H1-A acquisition overruns : 1\n', self.report))

    def test_missing_shutdown_rejected(self):
        self.assertTrue(verify_debug(self.log.replace('safe=1', 'safe=0'), self.report))

    def test_clock_lifecycle(self):
        log = self.log + ('State ACQUIRING -> LOCKED\n'
                          'State LOCKED -> HOLDOVER\n'
                          'State HOLDOVER -> LOCKED\n'
                          'sync=LOCKED gen=1 age=1ms utc_delta=2ms valid=0x07\n')
        report = dict(self.report, reconnections=1)
        self.assertEqual(verify_debug(log, report, require_clock=True), [])

    def test_production_recording(self):
        log = self.log + (
            'Segment recorded: raw=52 reflectance=13 dropped=0 rejected=0 '
            'write_errors=0 flushes=8 flush_errors=0 max_flush=12000us '
            'queue_hwm=3\n'
            'Segment recorded: raw=12 reflectance=3 dropped=0 rejected=0 '
            'write_errors=0 flushes=1 flush_errors=0 max_flush=9000us '
            'queue_hwm=2\n')
        self.assertEqual(verify_debug(log, self.report,
                                     require_recording=True), [])
        self.assertTrue(verify_debug(log.replace('dropped=0', 'dropped=1', 1),
                                     self.report, require_recording=True))
        self.assertTrue(verify_debug(log.replace('rejected=0', 'rejected=1', 1),
                                     self.report, require_recording=True))
        self.assertTrue(verify_debug(log.replace('flush_errors=0',
                                                 'flush_errors=1', 1),
                                     self.report, require_recording=True))

    def test_expected_recorder_pressure(self):
        log = self.log + (
            'TEST ONLY: injecting 8000 ms one-shot flush stall\n'
            'Segment recorded: raw=40 reflectance=9 dropped=8 rejected=0 '
            'write_errors=0 flushes=3 flush_errors=0 max_flush=8001000us '
            'queue_hwm=24\n'
            'Segment recorded: raw=12 reflectance=3 dropped=0 rejected=0 '
            'write_errors=0 flushes=2 flush_errors=0 max_flush=2000us '
            'queue_hwm=2\n')
        report = dict(self.report, events=[
            {'event': 'heartbeat', 'state': 2, 'error': 5}])
        self.assertEqual(verify_debug(log, report,
                                     require_recording=True,
                                     expect_pressure=True), [])
        self.assertTrue(verify_debug(log, self.report,
                                     require_recording=True,
                                     expect_pressure=True))
