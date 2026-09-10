from __future__ import annotations

from dataclasses import replace
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.request import urlopen
import zlib


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mission_viewer"))

from dji_h1_viewer import (  # noqa: E402
    GROUND, MissionService, RecordFormatError, build_mission_map,
    event_severity, open_mission, start_http_api,
)
from dji_h1_viewer.credentials import (  # noqa: E402
    CredentialError, load_baidu_map_ak,
)


FILE_HEADER = struct.Struct("<IHHII")
RECORD_HEADER = struct.Struct("<IHHIIIHHIQQQIHBB")


def _record(record_type: int, body: bytes, *, sequence: int = 1,
            b_monotonic_us: int | None = None,
            a_monotonic_ms: int | None = None,
            sync_generation: int = 3,
            time_valid_flags: int = 7) -> bytes:
    size = RECORD_HEADER.size + len(body) + 4
    header = RECORD_HEADER.pack(
        0x31524844, 1, record_type, RECORD_HEADER.size, size,
        0x12345678, 2, 0, sequence,
        b_monotonic_us if b_monotonic_us is not None else 1_000_000 + sequence,
        a_monotonic_ms if a_monotonic_ms is not None else 2_000 + sequence,
        1_725_000_000_000 + sequence, 5, sync_generation, 1,
        time_valid_flags)
    encoded = header + body
    return encoded + struct.pack("<I", zlib.crc32(encoded) & 0xFFFFFFFF)


def _write_file(path: Path, record_type: int, records: list[bytes]) -> None:
    path.write_bytes(FILE_HEADER.pack(0x31464844, 1, record_type, 16, 0)
                     + b"".join(records))


def _make_mission(root: Path) -> None:
    raw_prefix = struct.pack("<IIHhBBBB", 7, 5000, 4, 0, GROUND, 0, 1, 0)
    raw = _record(2, raw_prefix + struct.pack("<4H", 10, 20, 30, 40),
                  b_monotonic_us=1_050_000, a_monotonic_ms=2_050)
    _write_file(root / "RAW_SPECTRA.BIN", 2, [raw])

    reflectance_prefix = struct.pack(
        "<IIIQIHHHHHH", 9, 7, 8, 900_000, 25_000, 4, 4, 0, 1, 0, 15)
    reflectance = _record(
        3, reflectance_prefix + struct.pack("<4H", 100, 2500, 5000, 10000)
        + bytes((1, 1, 1, 5)), b_monotonic_us=1_075_000,
        a_monotonic_ms=2_075)
    _write_file(root / "REFLECTANCE.BIN", 3, [reflectance])

    gps_before = struct.pack(
        "<B3xiiiIIH8B", 11, 399_000_000, 1_164_000_000, 100_000,
        1_725_000_000, 2_000, 321, 3, 4, 2, 1, 6, 88, 0, 15)
    gps_after = struct.pack(
        "<B3xiiiIIH8B", 12, 399_000_100, 1_164_000_200, 120_000,
        1_725_000_000, 2_100, 421, 3, 4, 2, 1, 6, 87, 0, 15)
    _write_file(root / "GPS_TRACK.BIN", 1, [
        _record(1, gps_before, sequence=1, b_monotonic_us=1_010_000,
                a_monotonic_ms=2_003),
        _record(1, gps_after, sequence=2, b_monotonic_us=1_130_000,
                a_monotonic_ms=2_103),
    ])
    (root / "MISSION.JSON").write_text(
        json.dumps({
            "schema_version": 1,
            "flight_index": 42,
            "drone_serial": "TEST_DRONE",
            "drone_serial_hex": (
                "544553545F44524F4E45" + "00" * (32 - len("TEST_DRONE"))),
            "started_sync_generation": 3,
            "updated_sync_generation": 3,
        }), encoding="utf-8")
    (root / "EVENTS.JSONL").write_text(
        '{"sequence":1,"event":"handshake"}\n'
        '{"sequence":2,"event":"protocol_timeout",'
        '"b_monotonic_us":1050000,"utc_ms":1725000000100}\n'
        'not-json\n', encoding="utf-8")


class MissionViewerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        _make_mission(self.root)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_decodes_complete_mission_and_tolerates_bad_event_tail(self) -> None:
        mission = open_mission(self.root)
        self.assertEqual(mission.summary["flight_index"], 42)
        self.assertEqual(mission.raw_indices(role=GROUND), [0])
        self.assertEqual(mission.raw.raw_spectrum(0).samples, (10, 20, 30, 40))
        result = mission.reflectance.reflectance_spectrum(0)
        self.assertEqual(result.reflectance_0p01_percent[-1], 10000)
        self.assertEqual(len(mission.gps_page()), 2)
        self.assertEqual(mission.events[0]["event"], "handshake")
        self.assertEqual(mission.event_errors[0].line_number, 3)

    def test_isolates_crc_corruption_to_the_damaged_product(self) -> None:
        path = self.root / "RAW_SPECTRA.BIN"
        damaged = bytearray(path.read_bytes())
        damaged[-5] ^= 0x40
        path.write_bytes(damaged)
        mission = open_mission(self.root)
        self.assertIsNone(mission.raw)
        self.assertIsNotNone(mission.reflectance)
        self.assertEqual(mission.product_errors[0].filename, "RAW_SPECTRA.BIN")
        self.assertIn("CRC mismatch", mission.product_errors[0].message)

    def test_lazy_read_rechecks_crc_and_scan_identity(self) -> None:
        mission = open_mission(self.root)
        path = self.root / "RAW_SPECTRA.BIN"
        damaged = bytearray(path.read_bytes())
        damaged[-5] ^= 0x40
        path.write_bytes(damaged)
        with self.assertRaisesRegex(RecordFormatError,
                                    "changed or CRC mismatch"):
            mission.raw.raw_spectrum(0)

    def test_damaged_products_do_not_hide_mission_metadata(self) -> None:
        for name in ("RAW_SPECTRA.BIN", "REFLECTANCE.BIN", "GPS_TRACK.BIN"):
            (self.root / name).write_bytes(b"")
        mission = open_mission(self.root)
        self.assertEqual(mission.summary["flight_index"], 42)
        self.assertEqual(len(mission.product_errors), 3)
        self.assertIsNone(mission.raw)
        self.assertIsNone(mission.reflectance)
        self.assertIsNone(mission.gps)

    def test_service_returns_physical_reflectance_units(self) -> None:
        service = MissionService(open_mission(self.root))
        result = service.reflectance_spectrum(0)
        self.assertEqual(result["reflectance_percent"], [1.0, 25.0, 50.0, 100.0])
        self.assertAlmostEqual(result["position"]["interpolation_fraction"], 0.75)
        self.assertEqual(service.raw_index(role=GROUND)["total"], 1)

    def test_spectrum_position_uses_source_a_time_and_reports_provenance(self) -> None:
        mission = open_mission(self.root)
        located = mission.located_raw_spectrum(0)
        position = located.position
        self.assertIsNotNone(position)
        assert position is not None
        # GPS receive times would produce 1/3, so 1/2 proves that interpolation
        # used the GPS payload's source A timestamp rather than UART arrival.
        self.assertEqual(position.time_domain, "a_monotonic_ms")
        self.assertAlmostEqual(position.interpolation_fraction, 0.5)
        self.assertAlmostEqual(position.latitude_deg, 39.900005)
        self.assertAlmostEqual(position.longitude_deg, 116.40001)
        self.assertAlmostEqual(position.altitude_relative_m, 110.0)
        self.assertEqual(position.before_gps_index, 0)
        self.assertEqual(position.after_gps_index, 1)
        self.assertEqual(position.before_protocol_sequence, 11)
        self.assertEqual(position.after_protocol_sequence, 12)
        self.assertEqual(position.gap_ms, 100.0)
        self.assertEqual(position.quality, "interpolated")

    def test_interpolation_does_not_extrapolate_or_cross_an_optional_long_gap(self) -> None:
        mission = open_mission(self.root)
        header = mission.raw.records[0].header
        before_track = replace(header, a_monotonic_ms=1_999,
                               b_monotonic_us=999_000)
        self.assertIsNone(mission.position_for_header(before_track))
        self.assertIsNone(mission.position_for_header(header, max_gap_ms=99.0))

    def test_falls_back_to_b_time_without_a_clock_correlation(self) -> None:
        mission = open_mission(self.root)
        header = replace(mission.raw.records[0].header, time_valid_flags=0)
        position = mission.position_for_header(header)
        self.assertIsNotNone(position)
        assert position is not None
        self.assertEqual(position.time_domain, "b_monotonic_us")
        self.assertAlmostEqual(position.interpolation_fraction, 1.0 / 3.0)

    def test_builds_linked_map_layers_without_loading_spectrum_arrays(self) -> None:
        model = build_mission_map(open_mission(self.root))
        self.assertEqual(len(model.route), 2)
        self.assertEqual(len(model.measurements), 1)
        self.assertEqual(model.measurements[0].reflectance_index, 0)
        self.assertAlmostEqual(model.measurements[0].latitude_deg, 39.9000075)
        self.assertEqual(model.measurements[0].time_domain, "a_monotonic_ms")
        self.assertEqual(model.measurements[0].sync_generation, 3)
        self.assertEqual(len(model.events), 1)
        self.assertEqual(model.events[0].event_name, "protocol_timeout")
        self.assertEqual(model.events[0].severity, "critical")
        self.assertEqual(model.unlocated_measurements, 0)
        self.assertEqual(model.unlocated_events, 0)

    def test_marks_only_abnormal_stop_reasons_as_map_events(self) -> None:
        self.assertIsNone(event_severity(
            {"event": "stop_request", "argument1": 1}))
        self.assertEqual(event_severity(
            {"event": "stop_request", "argument1": 7}), "critical")
        self.assertEqual(event_severity(
            {"event": "stop_request", "argument1": 4}), "warning")
        self.assertEqual(event_severity(
            {"event": "stop_request", "argument1": 2}), "warning")
        self.assertEqual(event_severity(
            {"event": "capture_result", "argument1": -1}), "critical")
        self.assertIsNone(event_severity(
            {"event": "capture_result", "argument1": 0}))
        self.assertEqual(event_severity(
            {"event": "drone_identity_mismatch"}), "critical")

    def test_gui_smoke_loads_synthetic_mission_offscreen(self) -> None:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        try:
            from PyQt6.QtCore import pyqtSignal
            from PyQt6.QtWidgets import QApplication, QWidget
            from dji_h1_viewer.ui import MissionViewer
        except (ImportError, RuntimeError) as exc:
            self.skipTest(f"PyQt GUI runtime unavailable: {exc}")

        class OfflineMapStub(QWidget):
            """Avoid Chromium: this smoke test covers the Qt application shell."""

            measurement_selected = pyqtSignal(int)
            event_selected = pyqtSignal(int)
            status_changed = pyqtSignal(str)
            status_text = "Offline test map"
            baidu_available = False

            def set_model(self, model) -> None:
                self.model = model

            def select_measurement(self, _index: int, **_kwargs) -> None:
                pass

            def focus_event(self, _index: int) -> None:
                pass

            def fit_route(self) -> None:
                pass

        app = QApplication.instance() or QApplication([])
        with patch("dji_h1_viewer.ui.MissionMapPanel", OfflineMapStub):
            viewer = MissionViewer(
                MissionService(open_mission(self.root)), api_port=None)
        try:
            app.processEvents()
            self.assertEqual(len(viewer.map_model.route), 2)
            self.assertEqual(len(viewer.map_model.measurements), 1)
            self.assertIn("544553545F44524F4E45", viewer.flight_info.text())
            self.assertIn("A monotonic, sync generation 3",
                          viewer.spectrum_info.text())
            self.assertTrue(viewer.spectrum_plot._values)
        finally:
            viewer.close()
            app.processEvents()

    def test_loads_baidu_ak_only_from_a_valid_local_credential(self) -> None:
        credential = self.root / "baidu.local.json"
        credential.write_text('{"ak":"test_browser_ak_12345"}',
                              encoding="utf-8")
        self.assertEqual(load_baidu_map_ak(credential),
                         "test_browser_ak_12345")
        credential.write_text('{"ak":"bad key with spaces"}',
                              encoding="utf-8")
        with self.assertRaises(CredentialError):
            load_baidu_map_ak(credential)

    def test_baidu_template_uses_callback_loader_without_a_real_key(self) -> None:
        template = (ROOT / "tools" / "mission_viewer" / "dji_h1_viewer" /
                    "resources" / "baidu_map.html").read_text(encoding="utf-8")
        self.assertIn("callback=baiduApiReady", template)
        self.assertIn("script.async = true", template)
        self.assertIn("__BAIDU_MAP_AK__", template)
        self.assertIn("mapType: BMAP_SATELLITE_MAP", template)
        self.assertIn("showVectorStreetLayer: false", template)
        self.assertIn("showVectorLine: false", template)
        self.assertIn("map.setTrafficOff()", template)
        self.assertNotIn('<script src="https://api.map.baidu.com/api', template)
        source = (ROOT / "tools" / "mission_viewer" / "dji_h1_viewer" /
                  "baidu_map.py").read_text(encoding="utf-8")
        self.assertIn("LocalContentCanAccessFileUrls", source)
        self.assertRegex(source, r"LocalContentCanAccessFileUrls,\s+False")

    def test_read_only_http_api(self) -> None:
        service = MissionService(open_mission(self.root))
        server, thread = start_http_api(service, port=0)
        try:
            port = server.server_address[1]
            with urlopen(f"http://127.0.0.1:{port}/api/v1/mission") as response:
                payload = json.load(response)
            self.assertEqual(payload["files"]["raw"]["records"], 1)
            with urlopen(f"http://127.0.0.1:{port}/api/v1/raw?index=0") as response:
                payload = json.load(response)
            self.assertEqual(payload["samples"], [10, 20, 30, 40])
            self.assertAlmostEqual(payload["position"]["interpolation_fraction"], 0.5)
            with urlopen(
                    f"http://127.0.0.1:{port}/api/v1/position?b_monotonic_us=1050000"
                    ) as response:
                payload = json.load(response)
            self.assertAlmostEqual(payload["position"]["interpolation_fraction"],
                                   1.0 / 3.0)
            with urlopen(f"http://127.0.0.1:{port}/api/v1/map") as response:
                payload = json.load(response)
            self.assertEqual(len(payload["measurements"]), 1)
            self.assertEqual(payload["events"][0]["event_name"],
                             "protocol_timeout")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
