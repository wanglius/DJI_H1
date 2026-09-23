"""Chinese GUI labels must not change API/record values or map isolation."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from dji_h1_ground import MissionService
from dji_h1_ground.ui_zh import display_value, event_name
from test_mission_viewer import _make_mission


class ChineseGuiTests(unittest.TestCase):
    def test_labels_preserve_unknown_codes_and_template_language(self):
        self.assertEqual(display_value("subscribed"), "已订阅")
        self.assertEqual(event_name("ab_link_lost"), "A-B 链路丢失")
        self.assertIn("event_999", event_name("event_999"))
        self.assertEqual(display_value("future-state"), "future-state")
        template = (ROOT / "src/dji_h1_ground/resources/baidu_map.html").read_text(
            encoding="utf-8")
        self.assertIn('lang="zh-CN"', template)
        self.assertIn("正在加载百度地图", template)
        self.assertIn("item.event_label", template)

    def test_chinese_window_and_map_failure_do_not_mutate_api(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        try:
            from PyQt6.QtWidgets import QApplication
        except ImportError:
            self.skipTest("PyQt6 is not installed")
        from dji_h1_ground.ui import MissionViewer, _configure_chinese_ui
        app = QApplication.instance() or QApplication([])
        _configure_chinese_ui(app)
        with tempfile.TemporaryDirectory() as folder:
            _make_mission(Path(folder))
            service = MissionService()
            service.load(folder)
            before = json.dumps(service.events(), sort_keys=True)
            viewer = MissionViewer(service, api_port=None)
            try:
                self.assertEqual(viewer.windowTitle(), "DJI H1 地面站")
                self.assertEqual(viewer.open_button.text(), "打开任务文件夹…")
                self.assertEqual(viewer.live_button.text(), "连接实时遥测…")
                self.assertIn("反射率计算", viewer.spectrum_info.text())
                self.assertIn("飞行器", viewer.flight_info.text())
                viewer.route_map._web_error("external failure with secret URL")
                self.assertIn("数据接收继续运行", viewer.map_status.text())
                self.assertNotIn("secret", viewer.map_status.text())
                self.assertIs(viewer.route_map._stack.currentWidget(),
                              viewer.route_map._offline)
                self.assertEqual(json.dumps(service.events(), sort_keys=True), before)
                self.assertFalse(viewer.grab().isNull())
                app.processEvents()
            finally:
                viewer.close()
