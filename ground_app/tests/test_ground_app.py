"""Standalone journal, retry, API and map isolation integration tests."""
from dataclasses import replace
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from dji_h1_ground import GroundConfig, GroundReceiver, MapConfig, start_http_api
from dji_h1_ground.live import LiveReceiverConfig
from dji_h1_ground.telemetry import fragment_message
from dji_h1_ground.journal import iter_publications, export_jsonl
from test_live_viewer import _reflectance, _gps, _event


def wait_for(predicate):
    until = time.monotonic() + 4
    while not predicate():
        if time.monotonic() > until:
            raise AssertionError("worker did not finish")
        time.sleep(0.005)


class GroundAppTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.settings = GroundConfig(LiveReceiverConfig(host="unused.invalid"),
                                     storage_directory=Path(self.temp.name))

    def receiver(self, **kwargs):
        obj = GroundReceiver(self.settings, **kwargs)
        obj._transport = Mock()
        obj.start()
        self.addCleanup(obj.stop)
        return obj

    def feed(self, obj, kind=3, seq=1, data=None):
        encoded = b"".join(fragment_message(
            payload=data or _reflectance(seq, 2050), message_type=kind,
            source_id=11, mission_id=42, message_sequence=seq))
        obj._enqueue_publication(obj.config.uplink_topic, encoded, 1)

    def test_journal_output_ack_and_replay(self):
        obj = self.receiver()
        self.feed(obj)
        message = obj.get_message(timeout=3)
        self.assertIsNotNone(message)
        self.assertEqual(message.records[0].reflectance_0p01_percent[-1], 10000)
        self.assertGreater(message.received_utc_ns, 0)
        wait_for(lambda: obj.status()["acknowledgements"] == 1)
        obj.stop()
        journal = obj.status()["journal_path"]
        self.assertEqual(len(list(iter_publications(journal))), 1)
        replay = GroundReceiver(self.settings)
        replay._transport = Mock()
        self.assertEqual(list(replay.replay(journal)), [message])
        replay._transport.start.assert_not_called()
        replay._transport.publish.assert_not_called()
        output = Path(self.temp.name) / "decoded.jsonl"
        export_jsonl(journal, output)
        self.assertEqual(json.loads(output.read_text())["source_id_hex"], "000000000000000B")

    def test_disk_failure_withholds_ack_then_retry_succeeds(self):
        obj = self.receiver()
        with patch.object(obj._journal, "message", side_effect=OSError("disk full")):
            self.feed(obj)
            wait_for(lambda: obj.status()["journal_failures"] == 1)
            self.assertEqual(obj.status()["acknowledgements"], 0)
        self.feed(obj)
        self.assertIsNotNone(obj.get_message(timeout=3))
        wait_for(lambda: obj.status()["acknowledgements"] == 1)

    def test_filtered_wire_is_journaled_without_ack(self):
        self.settings = replace(self.settings, mqtt=replace(self.settings.mqtt, source_id=99))
        obj = self.receiver()
        self.feed(obj)
        self.feed(obj)
        wait_for(lambda: obj.status()["processed_publications"] == 2)
        self.assertEqual(obj.status()["acknowledgements"], 0)
        self.assertIsNone(obj.get_message(timeout=0))
        obj.stop()
        self.assertEqual(len(list(iter_publications(obj.status()["journal_path"]))), 2)

    def test_slow_output_does_not_block_ack(self):
        self.settings = replace(self.settings, output_queue_size=1)
        obj = self.receiver()
        for seq in range(1, 5):
            self.feed(obj, seq=seq)
        wait_for(lambda: obj.status()["acknowledgements"] == 4)
        self.assertEqual(obj.status()["output_dropped"], 3)
        self.assertEqual(obj.service.overview()["files"]["reflectance"]["records"], 4)

    def test_filtered_invalid_record_cannot_reject_another_devices_message(self):
        self.settings = replace(self.settings, mqtt=replace(self.settings.mqtt, source_id=99))
        obj = self.receiver()
        self.feed(obj, data=b"invalid DHR1 but valid transport CRC")
        wait_for(lambda: obj.status()["processed_publications"] == 1)
        self.assertEqual(obj.status()["filtered_messages"], 1)
        self.assertEqual(obj.status()["acknowledgements"], 0)

    def test_http_updates_without_gui_event_loop(self):
        obj = self.receiver()
        server, thread = start_http_api(obj.service, port=0)
        try:
            self.feed(obj)
            wait_for(lambda: obj.status()["complete_messages"] == 1)
            with urlopen(f"http://127.0.0.1:{server.server_port}/api/v1/reflectance?index=0") as response:
                self.assertEqual(json.load(response)["reflectance_percent"][-1], 100)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(2)

    def test_bad_map_config_nonfatal_and_repr_redacts_password(self):
        path = Path(self.temp.name) / "local.json"
        path.write_text(json.dumps({"schema_version": 1,
            "mqtt": {"host": "unused.invalid", "password": "sensitive-password"},
            "map": {"enabled": True, "ak": "invalid key"}}))
        config = GroundConfig.load(path)
        self.assertFalse(config.map.enabled)
        self.assertTrue(config.map.error)
        self.assertNotIn("sensitive-password", repr(config))

    def test_core_does_not_import_qt(self):
        code = "import sys; import dji_h1_ground; assert not any(k.startswith('PyQt') for k in sys.modules)"
        subprocess.run([sys.executable, "-c", code], check=True,
                       env={**os.environ, "PYTHONPATH": str(ROOT / "src")})

    def test_raw_gps_batch_and_event_outputs(self):
        from test_record_decoder import make_record, raw_body
        from dji_h1_ground import encode_gps_batch
        obj = self.receiver()
        self.feed(obj, 2, 5, make_record(2, raw_body(1, 2, 3), sequence=5))
        self.assertEqual(obj.get_message(timeout=3).records[0].samples, (1, 2, 3))
        self.feed(obj, 5, 10, encode_gps_batch([_gps(10, 2000, 399000000), _gps(11, 2200, 399000100)]))
        self.assertEqual(len(obj.get_message(timeout=3).records), 2)
        self.feed(obj, 4, 20, _event(20, 2100))
        self.assertEqual(obj.get_message(timeout=3).records[0].info.event_code, 13)

    def test_map_failure_preserves_reception_and_follow_latest(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        try:
            from PyQt6.QtWidgets import QApplication, QWidget
            from PyQt6.QtCore import pyqtSignal
            from dji_h1_ground.ui import MissionViewer
            from dji_h1_ground.api import MissionService
            from dji_h1_ground.map_panel import MissionMapPanel
        except ImportError:
            self.skipTest("optional PyQt6 not installed")

        class BrokenWeb(QWidget):
            map_ready = pyqtSignal()
            map_error = pyqtSignal(str)
            measurement_selected = pyqtSignal(int)
            event_selected = pyqtSignal(int)
            def set_model(self, *args, **kwargs):
                raise RuntimeError("map API failed")
            def stop(self):
                pass

        app = QApplication.instance() or QApplication([])
        settings = replace(self.settings, map=MapConfig(True, "test_key_12345", True))
        with patch.object(MissionMapPanel, "_create_web", lambda _self: BrokenWeb()):
            viewer = MissionViewer(MissionService(), api_port=None, ground_config=settings)
            app.processEvents()
            obj = self.receiver(output_enabled=False)
            viewer.live_source, viewer.service = obj, obj.service
            viewer.follow_latest.setChecked(True)
            self.feed(obj)
            wait_for(lambda: obj.status()["acknowledgements"] == 1)
            viewer.route_map._web.map_ready.emit()
            viewer._refresh_live()
            self.assertTrue(viewer.route_map._web_failed)
            self.assertTrue(viewer.spectrum_plot._values)
            self.feed(obj, seq=2)
            wait_for(lambda: obj.status()["acknowledgements"] == 2)
            viewer._refresh_live()
            self.assertIn("反射率计算 2", viewer.spectrum_info.text())
            self.assertEqual(obj.status()["journal_failures"], 0)
            viewer.close()
            app.processEvents()

    def test_map_missing_webengine_and_timeout_are_local(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        try:
            from PyQt6.QtWidgets import QApplication
            from dji_h1_ground.map_panel import MissionMapPanel
        except ImportError:
            self.skipTest("optional PyQt6 not installed")
        app = QApplication.instance() or QApplication([])
        with patch.object(MissionMapPanel, "_create_web", side_effect=ImportError("WebEngine missing")):
            panel = MissionMapPanel(config=MapConfig(True, "test_key_12345"))
            app.processEvents()
            self.assertTrue(panel._web_failed)
            self.assertIs(panel._stack.currentWidget(), panel._offline)
            panel._timer.timeout.emit()
            panel._web_ready()
            self.assertTrue(panel._web_failed)
            panel.close()

    def test_sixty_second_workload_all_full_spectra_gps_and_events(self):
        from test_live_viewer import _record
        from dji_h1_ground import encode_gps_batch
        obj = self.receiver(output_enabled=False)
        expected = 0
        for seq in range(1, 241):
            millis = seq * 250
            prefix = struct.pack("<IIIQIHHHHHH", seq, seq, seq, 900000,
                                 25000, 1024, 1024, 0, 0, 0, 15)
            data = _record(3, prefix + struct.pack("<1024H", *([5000] * 1024))
                           + bytes([1] * 1024), seq, 1000000 + millis * 1000, millis)
            self.feed(obj, 3, seq, data)
            expected += 1
            if seq % 8 == 0:
                first = (seq // 8 - 1) * 10 + 1
                samples = [_gps(i, i * 200, 399000000 + i) for i in range(first, first + 10)]
                self.feed(obj, 5, first, encode_gps_batch(samples))
                expected += 1
            if seq % 48 == 0:
                self.feed(obj, 4, seq, _event(seq, millis))
                expected += 1
            # Accelerated logical 60 s test; bound burst depth to avoid testing
            # an unrelated instantaneous ingress overflow instead of data flow.
            if obj.status()["ingress_queue_depth"] > 100:
                wait_for(lambda: obj.status()["ingress_queue_depth"] < 30)
        wait_for(lambda: obj.status()["acknowledgements"] == expected)
        self.assertEqual(obj.status()["reflectance_records"], 240)
        self.assertEqual(obj.status()["gps_records"], 300)
        self.assertEqual(obj.status()["event_records"], 5)
        self.assertEqual(obj.status()["ingress_dropped"], 0)
        self.assertEqual(obj.status()["journal_failures"], 0)

    def test_pending_map_timeout_and_renderer_termination(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        try:
            from PyQt6.QtWidgets import QApplication, QWidget
            from PyQt6.QtCore import pyqtSignal
            from dji_h1_ground.map_panel import MissionMapPanel
        except ImportError:
            self.skipTest("optional PyQt6 not installed")
        class PendingWeb(QWidget):
            map_ready = pyqtSignal()
            map_error = pyqtSignal(str)
            measurement_selected = pyqtSignal(int)
            event_selected = pyqtSignal(int)
            def stop(self):
                pass
        app = QApplication.instance() or QApplication([])
        with patch.object(MissionMapPanel, "_create_web", lambda _: PendingWeb()):
            panel = MissionMapPanel(config=MapConfig(True, "test_key_12345", True, 10))
            until = time.monotonic() + 1
            while not panel._web_failed and time.monotonic() < until:
                app.processEvents()
                time.sleep(0.005)
            self.assertTrue(panel._web_failed)
            self.assertIs(panel._stack.currentWidget(), panel._offline)
            panel.configure(MapConfig(True, "test_key_12345", True))
            app.processEvents()
            panel._web.map_ready.emit()
            panel._web.map_error.emit("renderer terminated")
            self.assertTrue(panel._web_failed)
            self.assertIs(panel._stack.currentWidget(), panel._offline)
            panel.close()
