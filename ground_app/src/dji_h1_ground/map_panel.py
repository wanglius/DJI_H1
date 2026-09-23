"""Map-only failure boundary; no receiver, storage or service ownership."""
from __future__ import annotations

from PyQt6.QtCore import QTimer, pyqtSignal
from PyQt6.QtWidgets import QWidget, QVBoxLayout, QStackedWidget

from .config import MapConfig


class MissionMapPanel(QWidget):
    measurement_selected = pyqtSignal(int)
    event_selected = pyqtSignal(int)
    status_changed = pyqtSignal(str)

    def __init__(self, parent=None, *, config: MapConfig | None = None):
        super().__init__(parent)
        from .ui import MissionMap
        self.config = config or MapConfig()
        self._stack = QStackedWidget(self)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._stack)
        self._offline = MissionMap()
        self._offline.measurement_selected.connect(self.measurement_selected.emit)
        self._offline.event_selected.connect(self.event_selected.emit)
        self._stack.addWidget(self._offline)
        self._web = None
        self._web_failed = False
        self._ready = False
        self._model = None
        self._selected_measurement = self._selected_event = None
        self._external_data_enabled = self.config.allow_mission_coordinates
        self.status_text = ("地图配置无效，已使用离线坐标图；接收不受影响"
                            if self.config.error else "离线坐标图")
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(lambda: self._web_error("Map loading timed out"))
        # Delay optional WebEngine creation until the rest of the app exists.
        if self.config.enabled and not self.config.error:
            QTimer.singleShot(0, self._load_web)

    def _create_web(self):
        from .baidu_map import BaiduMissionMap
        return BaiduMissionMap(ak=self.config.ak)

    def configure(self, config):
        """Apply newly selected local settings, containing all web teardown errors."""
        self._timer.stop()
        self._stack.setCurrentWidget(self._offline)
        old, self._web = self._web, None
        if old is not None:
            try:
                old.stop()
                old.map_ready.disconnect()
                old.map_error.disconnect()
                self._stack.removeWidget(old)
                old.deleteLater()
            except Exception:
                pass
        self.config = config
        self._web_failed = self._ready = False
        self._external_data_enabled = config.allow_mission_coordinates
        self.status_text = ("地图配置无效，已使用离线坐标图；接收不受影响"
                            if config.error else "离线坐标图")
        self.status_changed.emit(self.status_text)
        if config.enabled and not config.error:
            QTimer.singleShot(0, self._load_web)

    def _load_web(self):
        if self._web is not None or not self.config.enabled:
            return
        try:
            self._web = self._create_web()
            self._web.map_ready.connect(self._web_ready)
            self._web.map_error.connect(self._web_error)
            self._web.measurement_selected.connect(self.measurement_selected.emit)
            self._web.event_selected.connect(self.event_selected.emit)
            self._stack.addWidget(self._web)
            self.status_text = "正在加载百度地图；离线视图仍可使用"
            self.status_changed.emit(self.status_text)
            self._timer.start(self.config.load_timeout_ms)
        except Exception:
            self._web_error("Map initialization failed")

    def _web_error(self, _message):
        # Third-party error strings may contain URLs with AK; never display them.
        self._timer.stop()
        self._web_failed = True
        self._ready = False
        self._stack.setCurrentWidget(self._offline)
        self.status_text = "百度地图不可用，已切换到离线视图；数据接收继续运行"
        self.status_changed.emit(self.status_text)
        if self._web is not None:
            try:
                self._web.stop()
            except Exception:
                pass

    def _web_ready(self):
        # A timeout is terminal for this panel; a late JS callback must not
        # replace a usable offline view or repeatedly attempt a broken renderer.
        if self._web_failed:
            return
        self._timer.stop()
        self._ready = True
        self.status_text = "百度卫星图 · WGS84 坐标直传（未转换为 BD-09）"
        self.status_changed.emit(self.status_text)
        if self._external_data_enabled and self._model is not None:
            self._call_web("set_model", self._model)
            self._restore_selection()
        if not self._web_failed:
            self._stack.setCurrentWidget(self._web)

    def _call_web(self, method, *args, **kwargs):
        if self._web_failed or not self._ready or self._web is None:
            return
        try:
            getattr(self._web, method)(*args, **kwargs)
        except Exception:
            self._web_error("Map operation failed")

    def _restore_selection(self):
        if self._selected_measurement is not None:
            self._call_web("select_measurement", self._selected_measurement)
        elif self._selected_event is not None:
            self._call_web("focus_event", self._selected_event)

    @property
    def baidu_available(self):
        return self._ready and not self._web_failed

    def set_model(self, model, *, fit=True, reset_external_permission=True):
        self._model = model
        if reset_external_permission:
            self._selected_measurement = self._selected_event = None
        self._offline.set_model(model, fit=fit,
                                preserve_selection=not reset_external_permission)
        if self._external_data_enabled:
            self._call_web("set_model", model, fit=fit)
            self._restore_selection()

    def set_timezone(self, name, offset_minutes):
        self._offline.set_timezone(name, offset_minutes)

    def set_external_data_enabled(self, enabled):
        self._external_data_enabled = enabled
        if enabled and self._model is not None:
            self._call_web("set_model", self._model)
        elif not enabled:
            self._call_web("clear_mission")
            self._stack.setCurrentWidget(self._offline)

    def fit_route(self):
        self._offline.fit_route()
        self._call_web("fit_route")

    def select_measurement(self, index, *, center=False):
        self._selected_measurement, self._selected_event = index, None
        self._offline.select_measurement(index, center=center)
        if self._external_data_enabled:
            self._call_web("select_measurement", index, center=center)

    def focus_event(self, index):
        self._selected_measurement, self._selected_event = None, index
        self._offline.focus_event(index)
        if self._external_data_enabled:
            self._call_web("focus_event", index)
