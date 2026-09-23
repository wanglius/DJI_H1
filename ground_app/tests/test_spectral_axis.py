"""The confirmed 711-point H1 grid must not be inferred for other lengths."""
from dataclasses import asdict
import os
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from dji_h1_ground import MissionService, decode_record, spectrum_wavelengths_nm
from test_mission_viewer import _record


def make_spectrum(kind, count):
    values = struct.pack(f"<{count}H", *range(count))
    if kind == 2:
        body = struct.pack("<IIHhBBBB", 1, 5000, count, 0, 0, 0, 1, 0) + values
    else:
        body = (struct.pack("<IIIQIHHHHHH", 1, 1, 1, 900000, 100000,
                            count, count, 0, 0, 0, 15)
                + values + bytes([1] * count))
    return decode_record(_record(kind, body))


class SpectralAxisTests(unittest.TestCase):
    def test_endpoints_and_every_integer_wavelength(self):
        axis = spectrum_wavelengths_nm(711)
        self.assertEqual(axis, tuple(range(340, 1051)))
        self.assertEqual(axis[355], 695)
        for count in (0, 1, 64, 710, 712, 1024):
            self.assertIsNone(spectrum_wavelengths_nm(count))

    def test_raw_and_reflectance_api_keep_samples_aligned(self):
        for kind in (2, 3):
            for count in (4, 711):
                with self.subTest(kind=kind, count=count):
                    record = make_spectrum(kind, count)
                    expected = tuple(range(340, 1051)) if count == 711 else None
                    self.assertEqual(record.wavelengths_nm, expected)
                    # Derived properties do not change the canonical record JSON.
                    self.assertNotIn("wavelengths_nm", asdict(record))
                    mission = Mock()
                    located = SimpleNamespace(spectrum=record, position=None)
                    mission.located_raw_spectrum.return_value = located
                    mission.located_reflectance_spectrum.return_value = located
                    service = MissionService(mission)
                    result = (service.raw_spectrum(0) if kind == 2 else
                              service.reflectance_spectrum(0))
                    self.assertEqual(result["wavelengths_nm"],
                                     list(expected) if expected is not None else None)
                    values = result["samples"] if kind == 2 else result["reflectance_0p01_percent"]
                    self.assertEqual(values, list(range(count)))

    def test_plot_switches_between_wavelength_and_unknown_index(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        try:
            from PyQt6.QtWidgets import QApplication
        except ImportError:
            self.skipTest("PyQt6 is not installed")
        from dji_h1_ground.ui import SpectrumPlot
        app = QApplication.instance() or QApplication([])
        plot = SpectrumPlot()
        try:
            plot.resize(800, 300)
            plot.set_values([50.0] * 711)
            self.assertIn("340–1050", plot.axis_label())
            self.assertIn("nm", plot.axis_label())
            self.assertFalse(plot.grab().isNull())
            plot.set_values([50.0] * 64)
            self.assertIn("波长未知", plot.axis_label())
            self.assertFalse(plot.grab().isNull())
            app.processEvents()
        finally:
            plot.close()


if __name__ == "__main__":
    unittest.main()
