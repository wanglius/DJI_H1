"""Baidu Map JSAPI integration for the PyQt mission viewer."""

from __future__ import annotations

from dataclasses import asdict
import json
from pathlib import Path
from urllib.parse import quote

from PyQt6.QtCore import QObject, QUrl, pyqtSignal, pyqtSlot
from PyQt6.QtWebChannel import QWebChannel
from PyQt6.QtWebEngineCore import QWebEngineSettings
from PyQt6.QtWebEngineWidgets import QWebEngineView

from .credentials import load_baidu_map_ak
from .presentation import MissionMapModel


class _MapBridge(QObject):
    ready = pyqtSignal()
    error = pyqtSignal(str)
    measurement_selected = pyqtSignal(int)
    event_selected = pyqtSignal(int)

    @pyqtSlot()
    def mapReady(self) -> None:  # noqa: N802 - JavaScript bridge API
        self.ready.emit()

    @pyqtSlot(str)
    def mapError(self, message: str) -> None:  # noqa: N802
        self.error.emit(message)

    @pyqtSlot(int)
    def measurementSelected(self, index: int) -> None:  # noqa: N802
        self.measurement_selected.emit(index)

    @pyqtSlot(int)
    def eventSelected(self, index: int) -> None:  # noqa: N802
        self.event_selected.emit(index)


class BaiduMissionMap(QWebEngineView):
    """WebGL Baidu base map receiving compact mission layers from Python."""

    map_ready = pyqtSignal()
    map_error = pyqtSignal(str)
    measurement_selected = pyqtSignal(int)
    event_selected = pyqtSignal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._ready = False
        self._pending_payload: str | None = None
        self._bridge = _MapBridge(self)
        self._bridge.ready.connect(self._on_ready)
        self._bridge.error.connect(self.map_error.emit)
        self._bridge.measurement_selected.connect(
            self.measurement_selected.emit)
        self._bridge.event_selected.connect(self.event_selected.emit)
        self._channel = QWebChannel(self.page())
        self._channel.registerObject("bridge", self._bridge)
        self.page().setWebChannel(self._channel)
        # The page must load Baidu's remote API, but that third-party script
        # must not inherit Qt's default permission to read other file:// URLs.
        self.settings().setAttribute(
            QWebEngineSettings.WebAttribute.LocalContentCanAccessFileUrls,
            False)
        self.settings().setAttribute(
            QWebEngineSettings.WebAttribute.LocalContentCanAccessRemoteUrls,
            True)
        self.loadFinished.connect(self._load_finished)
        self.renderProcessTerminated.connect(
            lambda _status, _code: self.map_error.emit(
                "Baidu Map browser process terminated"))
        self._load_template()

    def _load_template(self) -> None:
        template_path = (Path(__file__).resolve().parent / "resources" /
                         "baidu_map.html")
        template = template_path.read_text(encoding="utf-8")
        # URL-encode the local credential before inserting it into the script
        # URL. The tracked HTML never contains a real AK.
        document = template.replace("__BAIDU_MAP_AK__",
                                    quote(load_baidu_map_ak(), safe=""))
        self.setHtml(document, QUrl.fromLocalFile(str(template_path.parent) + "/"))

    def _load_finished(self, success: bool) -> None:
        if not success:
            self.map_error.emit("Baidu Map page failed to load")

    def _on_ready(self) -> None:
        self._ready = True
        self.map_ready.emit()
        if self._pending_payload is not None:
            self._send_payload(self._pending_payload)

    @staticmethod
    def _payload(model: MissionMapModel) -> str:
        # This is intentionally WGS84 pass-through until the planned BD09
        # conversion is enabled. The UI labels that limitation explicitly.
        return json.dumps(asdict(model), ensure_ascii=True,
                          separators=(",", ":"))

    def set_model(self, model: MissionMapModel) -> None:
        self._pending_payload = self._payload(model)
        if self._ready:
            self._send_payload(self._pending_payload)

    def _send_payload(self, payload: str) -> None:
        self.page().runJavaScript(f"window.setMissionData({payload});")

    def clear_mission(self) -> None:
        self._pending_payload = None
        if self._ready:
            self.page().runJavaScript("window.clearMissionData();")

    def select_measurement(self, reflectance_index: int,
                           *, center: bool = False) -> None:
        del center
        if self._ready:
            self.page().runJavaScript(
                f"window.selectMeasurement({int(reflectance_index)});")

    def focus_event(self, event_index: int) -> None:
        if self._ready:
            self.page().runJavaScript(f"window.focusEvent({int(event_index)});")

    def fit_route(self) -> None:
        if self._ready:
            self.page().runJavaScript("window.fitMission();")
