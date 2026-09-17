from __future__ import annotations

import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from threading import Event
import time
from types import SimpleNamespace
from unittest.mock import patch
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mission_viewer"))

from dji_h1_viewer import (  # noqa: E402
    LiveConfigError, LiveMissionStore, LiveReceiverConfig, LiveTelemetrySource,
    MESSAGE_GPS, MESSAGE_GPS_BATCH, MESSAGE_OPERATION_LOG,
    MESSAGE_REFLECTANCE, MissionService,
    ReassembledTelemetry, decode_acknowledgement, decode_record,
    encode_gps_batch, fragment_message,
)


_RECORD_HEADER = struct.Struct("<IHHIIIHHIQQQIHBB")
_GPS_BODY = struct.Struct("<B3xiiiIIH8B")
_REFLECTANCE_PREFIX = struct.Struct("<IIIQIHHHHHH")
_OPERATION_EVENT_BODY = struct.Struct("<HBBIi")


def _record(record_type: int, body: bytes, sequence: int,
            b_us: int, a_ms: int) -> bytes:
    size = _RECORD_HEADER.size + len(body) + 4
    header = _RECORD_HEADER.pack(
        0x31524844, 1, record_type, _RECORD_HEADER.size, size,
        7, 2, 0, sequence, b_us, a_ms, 1_789_430_400_000 + a_ms,
        5, 3, 1, 7)
    without_crc = header + body
    return without_crc + struct.pack(
        "<I", zlib.crc32(without_crc) & 0xFFFFFFFF)


def _gps(sequence: int, a_ms: int, latitude_e7: int) -> bytes:
    body = _GPS_BODY.pack(
        sequence & 0xFF, latitude_e7, 1_164_000_000, 100_000,
        1_789_430_400, a_ms, a_ms % 1000,
        3, 4, 2, 1, 6, 88, 0, 15)
    return _record(MESSAGE_GPS, body, sequence,
                   1_000_000 + a_ms * 1000, a_ms)


def _reflectance(sequence: int, a_ms: int) -> bytes:
    body = (_REFLECTANCE_PREFIX.pack(
        sequence, 20, 21, 900_000, 25_000, 4, 4, 0, 0, 0, 15)
        + struct.pack("<4H", 100, 2500, 5000, 10000)
        + bytes((1, 1, 1, 1)))
    return _record(MESSAGE_REFLECTANCE, body, sequence,
                   1_000_000 + a_ms * 1000, a_ms)


def _event(sequence: int, a_ms: int, code: int = 13,
           severity: int = 3, argument0: int = 3000,
           argument1: int = 0) -> bytes:
    body = _OPERATION_EVENT_BODY.pack(
        code, severity, 0, argument0, argument1)
    return _record(MESSAGE_OPERATION_LOG, body, sequence,
                   1_000_000 + a_ms * 1000, a_ms)


def _message(message_type: int, sequence: int, payload: bytes) \
        -> ReassembledTelemetry:
    return ReassembledTelemetry(message_type, 0x112233445566, 42,
                                sequence, 0, 1, payload)


class LiveViewerTests(unittest.TestCase):
    @staticmethod
    def _wait_for(predicate, timeout: float = 2.0) -> None:
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise AssertionError("timed out waiting for live worker")
            time.sleep(0.005)

    def test_shared_decoder_validates_and_decodes_telemetry_record(self) -> None:
        decoded = decode_record(_reflectance(9, 2050),
                                expected_type=MESSAGE_REFLECTANCE,
                                expected_sequence=9)
        self.assertEqual(decoded.reflectance_0p01_percent,
                         (100, 2500, 5000, 10000))
        damaged = bytearray(_gps(1, 2000, 399_000_000))
        damaged[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC mismatch"):
            decode_record(damaged)

    def test_live_store_reuses_offline_position_interpolation(self) -> None:
        config = LiveReceiverConfig(host="localhost")
        store = LiveMissionStore(config)
        store.accept(_message(MESSAGE_GPS, 1,
                              _gps(1, 2000, 399_000_000)))
        store.accept(_message(MESSAGE_REFLECTANCE, 9,
                              _reflectance(9, 2050)))
        before = store.snapshot({"connected": True, "state": "subscribed"})
        self.assertIsNotNone(before)
        assert before is not None
        self.assertIsNone(before.located_reflectance_spectrum(0).position)

        store.accept(_message(MESSAGE_GPS, 2,
                              _gps(2, 2100, 399_000_100)))
        mission = store.snapshot({"connected": True, "state": "subscribed"})
        self.assertIsNotNone(mission)
        assert mission is not None
        located = mission.located_reflectance_spectrum(0)
        self.assertAlmostEqual(located.position.latitude_deg, 39.900005)
        service = MissionService()
        service.set_mission(mission, mode="live")
        self.assertEqual(service.mode, "live")
        self.assertEqual(service.reflectance_index()["total"], 1)
        self.assertEqual(service.gps()["total"], 2)

    def test_live_store_expands_lossless_gps_batch(self) -> None:
        store = LiveMissionStore(LiveReceiverConfig(host="localhost"))
        records = (_gps(10, 2000, 399_000_000),
                   _gps(11, 2200, 399_000_010))
        batch = encode_gps_batch(records)
        self.assertEqual(store.accept(
            _message(MESSAGE_GPS_BATCH, 10, batch)), 2)
        mission = store.snapshot({"connected": True, "state": "subscribed"})
        self.assertEqual(len(mission.gps), 2)

    def test_live_store_exposes_timestamped_operation_event(self) -> None:
        store = LiveMissionStore(LiveReceiverConfig(host="localhost"))
        self.assertEqual(store.accept(_message(
            MESSAGE_OPERATION_LOG, 4, _event(4, 2050))), 1)
        mission = store.snapshot({"connected": True, "state": "subscribed"})
        self.assertIsNotNone(mission)
        assert mission is not None
        self.assertEqual(len(mission.events), 1)
        self.assertEqual(mission.events[0]["event"], "ab_link_lost")
        self.assertEqual(mission.events[0]["severity"], "critical")
        self.assertEqual(mission.events[0]["argument0"], 3000)

        # Application retries are ACKed elsewhere but never duplicate the
        # event shown to the operator.
        self.assertEqual(store.accept(_message(
            MESSAGE_OPERATION_LOG, 4, _event(4, 2050))), 0)
        self.assertEqual(len(store.snapshot({}).events), 1)

    def test_local_live_configuration_is_validated(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "live_mqtt.local.json"
            path.write_text(json.dumps({
                "mqtt": {
                    "host": "broker.example", "ack_qos": 0,
                    "source_id": "0x112233445566",
                },
                "viewer": {
                    "timezone_name": "Asia/Shanghai",
                    "timezone_offset_minutes": 480,
                },
            }), encoding="utf-8")
            config = LiveReceiverConfig.load(path)
            self.assertEqual(config.host, "broker.example")
            self.assertEqual(config.source_id, 0x112233445566)
            self.assertEqual(config.ack_qos, 0)
            path.write_text('{"mqtt":{"host":"x","ack_qos":2}}',
                            encoding="utf-8")
            with self.assertRaises(LiveConfigError):
                LiveReceiverConfig.load(path)

    def test_receiver_acknowledges_before_any_gui_refresh(self) -> None:
        class FakeTransport:
            def __init__(self):
                self.publications = []

            def publish(self, topic, payload, qos):
                self.publications.append((topic, payload, qos))

        source = LiveTelemetrySource(LiveReceiverConfig(host="localhost"))
        transport = FakeTransport()
        source._transport = transport
        source._accepting = True
        source._start_workers()
        wire, = fragment_message(
            MESSAGE_REFLECTANCE, 42, 9, _reflectance(9, 2050),
            source_id=0x112233445566)
        try:
            source._enqueue_publication("dji-h1/test/up", wire, 1)
            self._wait_for(
                lambda: source.status()["acknowledgements"] == 1)
            status = source.status()
            self.assertEqual(status["complete_messages"], 1)
            self.assertEqual(status["reflectance_records"], 1)
            self.assertEqual(len(transport.publications), 1)
            acknowledgement = decode_acknowledgement(
                transport.publications[0][1])
            self.assertEqual(acknowledgement.message_sequence, 9)
            self.assertEqual(acknowledgement.status, 0)

            # A B-board retry is not inserted twice, but it receives the
            # cached DTA1 even though Qt has never requested a snapshot.
            source._enqueue_publication("dji-h1/test/up", wire, 1)
            self._wait_for(
                lambda: source.status()["acknowledgements"] == 2)
            self.assertEqual(source.status()["duplicate_fragments"], 1)
            self.assertEqual(len(transport.publications), 2)
            self.assertEqual(len(source.snapshot().reflectance), 1)
        finally:
            source._accepting = False
            source._stop_workers()

    def test_receiver_does_not_ack_filtered_identity(self) -> None:
        class FakeTransport:
            def __init__(self):
                self.publications = []

            def publish(self, topic, payload, qos):
                self.publications.append((topic, payload, qos))

        source = LiveTelemetrySource(LiveReceiverConfig(
            host="localhost", source_id=0x111111111111))
        transport = FakeTransport()
        source._transport = transport
        source._accepting = True
        source._start_workers()
        wire, = fragment_message(
            MESSAGE_REFLECTANCE, 42, 9, _reflectance(9, 2050),
            source_id=0x222222222222)
        try:
            source._enqueue_publication("dji-h1/test/up", wire, 1)
            self._wait_for(
                lambda: source.status()["processed_publications"] == 1)
            status = source.status()
            self.assertEqual(status["acknowledgements"], 0)
            self.assertEqual(status["complete_messages"], 0)
            self.assertEqual(source.store.filtered_records, 1)
            self.assertIsNone(source.snapshot())
            self.assertEqual(transport.publications, [])
        finally:
            source._accepting = False
            source._stop_workers()

    def test_receiver_decodes_and_acknowledges_operation_event(self) -> None:
        class FakeTransport:
            def __init__(self):
                self.publications = []

            def publish(self, topic, payload, qos):
                self.publications.append((topic, payload, qos))

        source = LiveTelemetrySource(LiveReceiverConfig(host="localhost"))
        transport = FakeTransport()
        source._transport = transport
        source._accepting = True
        source._start_workers()
        wire, = fragment_message(
            MESSAGE_OPERATION_LOG, 42, 4, _event(4, 2050),
            source_id=0x112233445566)
        try:
            source._enqueue_publication("dji-h1/test/up", wire, 1)
            self._wait_for(
                lambda: source.status()["acknowledgements"] == 1)
            status = source.status()
            self.assertEqual(status["complete_messages"], 1)
            self.assertEqual(status["event_records"], 1)
            acknowledgement = decode_acknowledgement(
                transport.publications[0][1])
            self.assertEqual(acknowledgement.message_sequence, 4)
            self.assertEqual(acknowledgement.status, 0)
            mission = source.snapshot()
            self.assertEqual(len(mission.events), 1)
            self.assertEqual(mission.events[0]["event"], "ab_link_lost")
            self.assertEqual(mission.events[0]["severity"], "critical")
        finally:
            source._accepting = False
            source._stop_workers()

    def test_mqtt_callback_is_not_blocked_by_decoder(self) -> None:
        source = LiveTelemetrySource(LiveReceiverConfig(
            host="localhost", ingress_queue_size=4))
        entered = Event()
        release = Event()

        def slow_processor(_publication):
            entered.set()
            release.wait(2.0)

        source._process_publication = slow_processor
        source._accepting = True
        source._start_workers()
        message = SimpleNamespace(
            payload=b"first", qos=1, topic="dji-h1/test/up")
        try:
            source._transport._on_message(None, None, message)
            self.assertTrue(entered.wait(1.0))
            message.payload = b"second"
            started = time.perf_counter()
            source._transport._on_message(None, None, message)
            elapsed = time.perf_counter() - started
            self.assertLess(elapsed, 0.05)
            self.assertEqual(source.status()["ingress_queue_depth"], 1)
        finally:
            release.set()
            source._accepting = False
            source._stop_workers()

    def test_ingress_pressure_is_counted_without_fake_ack(self) -> None:
        source = LiveTelemetrySource(LiveReceiverConfig(
            host="localhost", ingress_queue_size=1))
        source._accepting = True
        source._enqueue_publication("dji-h1/test/up", b"one", 1)
        source._enqueue_publication("dji-h1/test/up", b"two", 1)
        status = source.status()
        self.assertEqual(status["mqtt_messages"], 2)
        self.assertEqual(status["ingress_dropped"], 1)
        self.assertEqual(status["acknowledgements"], 0)

    def test_slow_ack_publish_does_not_block_record_processing(self) -> None:
        entered = Event()
        release = Event()

        class SlowTransport:
            def __init__(self):
                self.publications = 0

            def publish(self, _topic, _payload, _qos):
                entered.set()
                release.wait(2.0)
                self.publications += 1

        source = LiveTelemetrySource(LiveReceiverConfig(
            host="localhost", ack_queue_size=4))
        transport = SlowTransport()
        source._transport = transport
        source._accepting = True
        source._start_workers()
        first, = fragment_message(
            MESSAGE_REFLECTANCE, 42, 9, _reflectance(9, 2050),
            source_id=0x112233445566)
        second, = fragment_message(
            MESSAGE_REFLECTANCE, 42, 10, _reflectance(10, 2150),
            source_id=0x112233445566)
        try:
            source._enqueue_publication("dji-h1/test/up", first, 1)
            self.assertTrue(entered.wait(1.0))
            source._enqueue_publication("dji-h1/test/up", second, 1)
            self._wait_for(
                lambda: source.status()["complete_messages"] == 2)
            self.assertEqual(source.status()["ack_queue_depth"], 1)
            self.assertEqual(source.status()["acknowledgements"], 0)
            release.set()
            self._wait_for(
                lambda: source.status()["acknowledgements"] == 2)
            self.assertEqual(transport.publications, 2)
        finally:
            release.set()
            source._accepting = False
            source._stop_workers()

    def test_one_window_renders_a_live_snapshot(self) -> None:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        try:
            from PyQt6.QtCore import pyqtSignal
            from PyQt6.QtWidgets import QApplication, QWidget
            from dji_h1_viewer.ui import MissionViewer
        except (ImportError, RuntimeError) as exc:
            self.skipTest(f"PyQt GUI runtime unavailable: {exc}")

        class MapStub(QWidget):
            measurement_selected = pyqtSignal(int)
            event_selected = pyqtSignal(int)
            status_changed = pyqtSignal(str)
            status_text = "Live test map"
            baidu_available = False

            def set_model(self, model, **_kwargs):
                self.model = model

            def set_timezone(self, name, offset):
                self.timezone = (name, offset)

            def select_measurement(self, _index, **_kwargs):
                pass

            def focus_event(self, _index):
                pass

            def fit_route(self):
                pass

        source = LiveTelemetrySource(LiveReceiverConfig(host="localhost"))
        source.store.accept(_message(
            MESSAGE_GPS, 1, _gps(1, 2000, 399_000_000)))
        source.store.accept(_message(
            MESSAGE_REFLECTANCE, 9, _reflectance(9, 2050)))
        source.store.accept(_message(
            MESSAGE_GPS, 2, _gps(2, 2100, 399_000_100)))
        source.store.accept(_message(
            MESSAGE_OPERATION_LOG, 4, _event(4, 2050)))
        source.store.accept(_message(
            MESSAGE_OPERATION_LOG, 5, _event(5, 5000)))
        source._status.update({"connected": True, "state": "subscribed"})
        app = QApplication.instance() or QApplication([])
        with patch("dji_h1_viewer.ui.MissionMapPanel", MapStub):
            viewer = MissionViewer(MissionService(), api_port=None)
        try:
            viewer.live_source = source
            viewer.follow_latest.setEnabled(True)
            viewer._refresh_live()
            app.processEvents()
            self.assertEqual(viewer.service.mode, "live")
            self.assertEqual(len(viewer.map_model.route), 2)
            self.assertEqual(len(viewer.map_model.measurements), 1)
            self.assertEqual(len(viewer.map_model.events), 1)
            self.assertEqual(viewer.events_list.count(), 2)
            self.assertIn("location pending", viewer.events_list.item(1).text())
            self.assertIn("Reflectance calculation 9",
                          viewer.spectrum_info.text())
            self.assertIn("Live link", viewer.flight_info.text())
        finally:
            viewer.close()
            app.processEvents()


if __name__ == "__main__":
    unittest.main()
