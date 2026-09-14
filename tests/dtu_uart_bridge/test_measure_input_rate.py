"""Unit tests for the hardware-independent input-rate probe helpers."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import unittest


THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))

from measure_input_rate import (  # noqa: E402
    GPS_PAYLOAD_BYTES,
    DEFAULT_REFLECTANCE_SAMPLES,
    Stage,
    _build_report,
    parse_rates,
    percentile,
    reflectance_payload_bytes,
    synthetic_payload,
)
from dji_h1_viewer.telemetry import (  # noqa: E402
    MESSAGE_GPS,
    MESSAGE_REFLECTANCE,
    fragment_message,
)


class InputRateProbeTests(unittest.TestCase):
    def test_production_sizes_have_expected_fragment_counts(self) -> None:
        gps = synthetic_payload(GPS_PAYLOAD_BYTES, 1, MESSAGE_GPS, 2)
        reflectance = synthetic_payload(
            reflectance_payload_bytes(DEFAULT_REFLECTANCE_SAMPLES),
            1, MESSAGE_REFLECTANCE, 3,
        )

        self.assertEqual(len(gps), 98)
        self.assertEqual(len(reflectance), 2233)
        self.assertEqual(len(fragment_message(MESSAGE_GPS, 1, 2, gps)), 1)
        self.assertEqual(
            len(fragment_message(MESSAGE_REFLECTANCE, 1, 3, reflectance)), 3
        )
        self.assertEqual(reflectance_payload_bytes(1024), 3172)

    def test_rate_parser_rejects_ambiguous_sweeps(self) -> None:
        self.assertEqual(parse_rates("0, 1,2.5"), (0.0, 1.0, 2.5))
        for invalid in ("", "1,1", "-1", "nan", "101"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(argparse.ArgumentTypeError):
                    parse_rates(invalid)

    def test_nearest_rank_percentile(self) -> None:
        self.assertIsNone(percentile([], 95))
        self.assertEqual(percentile([4, 1, 3, 2], 50), 2)
        self.assertEqual(percentile([4, 1, 3, 2], 95), 4)

    def test_report_fails_only_the_under_threshold_stage(self) -> None:
        stage = Stage(0, 100, 0, 0, 10)
        payload = synthetic_payload(20, 100, MESSAGE_GPS, 7)
        from measure_input_rate import Offer
        offers = {
            (100, MESSAGE_GPS, 7): Offer(
                0, MESSAGE_GPS, 7, payload, 1.0, 1, 68, 0.0,
            ),
            (100, MESSAGE_GPS, 8): Offer(
                0, MESSAGE_GPS, 8, payload, 2.0, 1, 68, 0.0,
            ),
        }

        report, passed = _build_report(
            [stage], offers, {(100, MESSAGE_GPS, 7): 1.5},
            1, 68, 1, 0, 2.0, 99.0,
        )

        self.assertFalse(passed)
        self.assertFalse(report["stages"][0]["passed"])
        gps = report["stages"][0]["messages"]["gps"]
        self.assertEqual(gps["delivery_percent"], 50.0)
        self.assertEqual(gps["missing"], 1)


if __name__ == "__main__":
    unittest.main()
