"""Host checks for the debug/heartbeat cross-check (does not open UARTs)."""
import unittest
from run_hardware_flight import verify_debug


class HardwareReportTests(unittest.TestCase):
    def setUp(self):
        self.log = ('H1-A frames OK : 4\nH1-B frames OK : 5\n'
                    'H1-A frames OK : 2\nH1-B frames OK : 3\n'
                    'Shutdown complete: safe=1 result=ESP_OK\n')
        self.report = {'session_final_counts': {11: 9, 12: 5}}

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
