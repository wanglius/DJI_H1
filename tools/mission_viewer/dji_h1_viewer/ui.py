"""PyQt6 desktop viewer linking a flight map to reflectance records."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import html
import math
import os
from pathlib import Path
import sys
from typing import Sequence

# The map does not use WebRTC. Prevent Chromium from probing public STUN
# servers or exposing a direct UDP interface; preserve any caller-supplied
# Chromium flags while adding this narrowly scoped network policy.
_WEBRTC_POLICY = "--force-webrtc-ip-handling-policy=disable_non_proxied_udp"
_chromium_flags = os.environ.get("QTWEBENGINE_CHROMIUM_FLAGS", "")
if _WEBRTC_POLICY not in _chromium_flags.split():
    os.environ["QTWEBENGINE_CHROMIUM_FLAGS"] = (
        f"{_chromium_flags} {_WEBRTC_POLICY}".strip())

from PyQt6.QtCore import QPointF, QRectF, Qt, pyqtSignal
from PyQt6.QtGui import (
    QBrush, QCloseEvent, QColor, QFont, QFontDatabase, QMouseEvent, QPainter,
    QPainterPath, QPen, QWheelEvent,
)
from PyQt6.QtWidgets import (
    QApplication, QFileDialog, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
    QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QPushButton,
    QScrollArea, QSizePolicy, QSplitter, QStackedWidget, QStatusBar, QToolTip,
    QVBoxLayout, QWidget,
)

from .api import MissionHttpServer, MissionService, start_http_api
from .baidu_map import BaiduMissionMap
from .credentials import CredentialError
from .presentation import (
    MeasurementPoint, MissionEventPoint, MissionMapModel, build_mission_map,
)


def _nice_distance(value: float) -> float:
    """Round a positive distance to a legible 1/2/5 × 10^n value."""

    if not math.isfinite(value) or value <= 0:
        return 1.0
    exponent = math.floor(math.log10(value))
    fraction = value / (10 ** exponent)
    factor = (1 if fraction < 1.5 else 2 if fraction < 3.5
              else 5 if fraction < 7.5 else 10)
    return factor * (10 ** exponent)


def _time_text(utc_ms: int, offset_minutes: int = 0,
               timezone_name: str = "UTC") -> str:
    if utc_ms <= 0:
        return "unavailable"
    local_zone = timezone(timedelta(minutes=offset_minutes))
    local = datetime.fromtimestamp(utc_ms / 1000, local_zone).strftime(
        "%Y-%m-%d %H:%M:%S.%f")[:-3]
    return (f"{local} {timezone_name} "
            f"({_offset_text(offset_minutes)})")


def _offset_text(offset_minutes: int) -> str:
    sign = "+" if offset_minutes >= 0 else "-"
    magnitude = abs(offset_minutes)
    return f"UTC{sign}{magnitude // 60:02d}:{magnitude % 60:02d}"


def _time_domain_text(domain: str, generation: int | None) -> str:
    if domain == "a_monotonic_ms":
        suffix = (f", sync generation {generation}"
                  if generation is not None else "")
        return f"A monotonic{suffix}"
    if domain == "b_monotonic_us":
        return "B monotonic fallback"
    return domain


def _configure_application_font(app: QApplication) -> None:
    """Select a stable UI font, including Qt's headless Windows backend."""

    families = QFontDatabase.families()
    if "Segoe UI" not in families:
        windows_font = Path(r"C:\Windows\Fonts\segoeui.ttf")
        if windows_font.is_file():
            font_id = QFontDatabase.addApplicationFont(str(windows_font))
            if font_id >= 0:
                families = QFontDatabase.applicationFontFamilies(font_id)
    app.setFont(QFont("Segoe UI" if "Segoe UI" in families else
                      families[0] if families else "Sans Serif", 9))


class SpectrumPlot(QWidget):
    """Dependency-free Qt line plot for a selected reflectance spectrum."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._values: tuple[float, ...] = ()
        self._message = "Select a measurement point on the route"
        self.setMinimumHeight(220)
        self.setSizePolicy(QSizePolicy.Policy.Expanding,
                           QSizePolicy.Policy.Expanding)

    def set_values(self, values: Sequence[float]) -> None:
        self._values = tuple(float(value) for value in values)
        self._message = ""
        self.update()

    def clear(self, message: str) -> None:
        self._values = ()
        self._message = message
        self.update()

    def paintEvent(self, _event) -> None:  # noqa: N802 - Qt API
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        palette = self.palette()
        painter.fillRect(self.rect(), palette.color(palette.ColorRole.Base))
        foreground = palette.color(palette.ColorRole.Text)
        muted = palette.color(palette.ColorRole.Mid)
        grid = palette.color(palette.ColorRole.Midlight)
        accent = QColor("#168aad")

        if not self._values:
            painter.setPen(muted)
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter,
                             self._message or "No spectrum available")
            return

        left, right, top, bottom = 70.0, 20.0, 18.0, 48.0
        plot = QRectF(left, top, max(1.0, self.width() - left - right),
                      max(1.0, self.height() - top - bottom))
        painter.setPen(QPen(grid, 1))
        for tick in range(5):
            fraction = tick / 4
            y = plot.bottom() - fraction * plot.height()
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y))
            painter.setPen(muted)
            painter.drawText(QRectF(4, y - 9, left - 12, 18),
                             Qt.AlignmentFlag.AlignRight |
                             Qt.AlignmentFlag.AlignVCenter,
                             f"{fraction * 100:.0f}")
            painter.setPen(QPen(grid, 1))

        painter.setPen(QPen(muted, 1))
        painter.drawRect(plot)
        count = len(self._values)
        denominator = max(1, count - 1)
        path = QPainterPath()
        for index, value in enumerate(self._values):
            x = plot.left() + index / denominator * plot.width()
            y = (plot.bottom() - min(100.0, max(0.0, value)) /
                 100.0 * plot.height())
            if index == 0:
                path.moveTo(x, y)
            else:
                path.lineTo(x, y)
        painter.setPen(QPen(accent, 1.6))
        painter.drawPath(path)

        painter.setPen(foreground)
        painter.drawText(QRectF(plot.left(), plot.bottom() + 8,
                                plot.width(), 24),
                         Qt.AlignmentFlag.AlignCenter,
                         f"sample index (0–{count - 1})")
        painter.save()
        painter.translate(18, plot.center().y())
        painter.rotate(-90)
        painter.drawText(QRectF(-plot.height() / 2, -12,
                                plot.height(), 24),
                         Qt.AlignmentFlag.AlignCenter, "reflectance (%)")
        painter.restore()


class MissionMap(QWidget):
    """Offline local projection with wheel zoom and linked point selection."""

    measurement_selected = pyqtSignal(int)
    event_selected = pyqtSignal(int)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._model = MissionMapModel((), (), (), 0, 0)
        self._reference_latitude = 0.0
        self._reference_longitude = 0.0
        self._longitude_scale = 111_320.0
        self._route: list[tuple[float, float]] = []
        self._measurements: list[tuple[float, float, MeasurementPoint]] = []
        self._events: list[tuple[float, float, MissionEventPoint]] = []
        self._center_x = 0.0
        self._center_y = 0.0
        self._scale = 1.0
        self._selected_measurement: int | None = None
        self._selected_event: int | None = None
        self._pan_anchor: QPointF | None = None
        self._timezone_name = "UTC"
        self._timezone_offset_minutes = 0
        self.setMinimumSize(520, 360)
        self.setMouseTracking(True)

    def set_timezone(self, name: str, offset_minutes: int) -> None:
        self._timezone_name = name
        self._timezone_offset_minutes = offset_minutes

    def set_model(self, model: MissionMapModel) -> None:
        self._model = model
        source = list(model.route) or list(model.measurements)
        if source:
            self._reference_latitude = sum(
                point.latitude_deg for point in source) / len(source)
            self._reference_longitude = sum(
                point.longitude_deg for point in source) / len(source)
            self._longitude_scale = 111_320.0 * math.cos(
                math.radians(self._reference_latitude))
        self._route = [self._project(point.latitude_deg, point.longitude_deg)
                       for point in model.route]
        self._measurements = [(*self._project(point.latitude_deg,
                                               point.longitude_deg), point)
                              for point in model.measurements]
        self._events = [(*self._project(point.latitude_deg,
                                        point.longitude_deg), point)
                        for point in model.events]
        self._selected_measurement = None
        self._selected_event = None
        self.fit_route()

    def _project(self, latitude: float, longitude: float) -> tuple[float, float]:
        return ((longitude - self._reference_longitude) * self._longitude_scale,
                (latitude - self._reference_latitude) * 111_320.0)

    def _to_screen(self, x: float, y: float) -> QPointF:
        return QPointF((x - self._center_x) * self._scale + self.width() / 2,
                       self.height() / 2 - (y - self._center_y) * self._scale)

    def _to_world(self, point: QPointF) -> tuple[float, float]:
        return ((point.x() - self.width() / 2) / self._scale + self._center_x,
                -(point.y() - self.height() / 2) / self._scale + self._center_y)

    def fit_route(self) -> None:
        points = self._route or [(x, y) for x, y, _ in self._measurements]
        if not points:
            self._center_x = self._center_y = 0.0
            self._scale = 1.0
            self.update()
            return
        xs, ys = zip(*points)
        xmin, xmax, ymin, ymax = min(xs), max(xs), min(ys), max(ys)
        span_x = max(20.0, xmax - xmin)
        span_y = max(20.0, ymax - ymin)
        self._center_x = (xmin + xmax) / 2
        self._center_y = (ymin + ymax) / 2
        self._scale = max(
            0.01, min((max(100, self.width()) - 90) / span_x,
                      (max(100, self.height()) - 90) / span_y))
        self.update()

    def select_measurement(self, reflectance_index: int,
                           *, center: bool = False) -> None:
        self._selected_measurement = reflectance_index
        self._selected_event = None
        if center:
            for x, y, point in self._measurements:
                if point.reflectance_index == reflectance_index:
                    self._center_x, self._center_y = x, y
                    break
        self.update()

    def focus_event(self, event_index: int) -> None:
        self._selected_event = event_index
        for x, y, point in self._events:
            if point.event_index == event_index:
                self._center_x, self._center_y = x, y
                break
        self.update()

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802 - Qt API
        if not self._route and not self._measurements:
            return
        anchor = event.position()
        before_x, before_y = self._to_world(anchor)
        factor = 1.25 ** (event.angleDelta().y() / 120.0)
        self._scale = min(5000.0, max(0.005, self._scale * factor))
        after_x, after_y = self._to_world(anchor)
        self._center_x += before_x - after_x
        self._center_y += before_y - after_y
        self.update()
        event.accept()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.RightButton:
            self._pan_anchor = event.position()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            event.accept()
            return
        if event.button() != Qt.MouseButton.LeftButton:
            return
        measurement = self._nearest_measurement(event.position(), 12.0)
        if measurement is not None:
            self.select_measurement(measurement.reflectance_index)
            self.measurement_selected.emit(measurement.reflectance_index)
            return
        map_event = self._nearest_event(event.position(), 12.0)
        if map_event is not None:
            self.focus_event(map_event.event_index)
            self.event_selected.emit(map_event.event_index)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if self._pan_anchor is not None:
            delta = event.position() - self._pan_anchor
            self._center_x -= delta.x() / self._scale
            self._center_y += delta.y() / self._scale
            self._pan_anchor = event.position()
            self.update()
            return
        measurement = self._nearest_measurement(event.position(), 9.0)
        map_event = self._nearest_event(event.position(), 9.0)
        if measurement is not None:
            self.setCursor(Qt.CursorShape.PointingHandCursor)
            QToolTip.showText(
                event.globalPosition().toPoint(),
                f"Reflectance #{measurement.calculation_count}\n"
                f"{measurement.latitude_deg:.7f}, "
                f"{measurement.longitude_deg:.7f}\n"
                f"{_time_text(measurement.utc_ms, self._timezone_offset_minutes, self._timezone_name)}\n"
                f"{_time_domain_text(measurement.time_domain, measurement.sync_generation)}",
                self)
        elif map_event is not None:
            self.setCursor(Qt.CursorShape.PointingHandCursor)
            QToolTip.showText(event.globalPosition().toPoint(),
                              map_event.event_name.replace("_", " "), self)
        else:
            self.unsetCursor()
            QToolTip.hideText()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.RightButton:
            self._pan_anchor = None
            self.unsetCursor()
            event.accept()

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self.fit_route()

    def leaveEvent(self, _event) -> None:  # noqa: N802
        if self._pan_anchor is None:
            self.unsetCursor()
        QToolTip.hideText()

    def _nearest_measurement(self, cursor: QPointF,
                             radius: float) -> MeasurementPoint | None:
        nearest: MeasurementPoint | None = None
        best = radius * radius
        for x, y, point in self._measurements:
            screen = self._to_screen(x, y)
            distance = ((screen.x() - cursor.x()) ** 2 +
                        (screen.y() - cursor.y()) ** 2)
            if distance <= best:
                best, nearest = distance, point
        return nearest

    def _nearest_event(self, cursor: QPointF,
                       radius: float) -> MissionEventPoint | None:
        nearest: MissionEventPoint | None = None
        best = radius * radius
        for x, y, point in self._events:
            screen = self._to_screen(x, y)
            distance = ((screen.x() - cursor.x()) ** 2 +
                        (screen.y() - cursor.y()) ** 2)
            if distance <= best:
                best, nearest = distance, point
        return nearest

    def paintEvent(self, _event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        palette = self.palette()
        background = palette.color(palette.ColorRole.Base)
        foreground = palette.color(palette.ColorRole.Text)
        muted = palette.color(palette.ColorRole.Mid)
        grid = palette.color(palette.ColorRole.Midlight)
        painter.fillRect(self.rect(), background)

        if not self._route and not self._measurements:
            painter.setPen(muted)
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter,
                             "Open a mission folder to view its flight route")
            return

        self._draw_grid(painter, grid, muted)
        if len(self._route) > 1:
            path = QPainterPath(self._to_screen(*self._route[0]))
            for point in self._route[1:]:
                path.lineTo(self._to_screen(*point))
            painter.setPen(QPen(QColor("#6c7a89"), 1.4))
            painter.drawPath(path)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QBrush(QColor("#97a3ad")))
        for x, y in self._route:
            painter.drawEllipse(self._to_screen(x, y), 1.5, 1.5)

        for x, y, point in self._measurements:
            screen = self._to_screen(x, y)
            selected = point.reflectance_index == self._selected_measurement
            if selected:
                painter.setPen(QPen(QColor("#ffca3a"), 2.5))
                painter.setBrush(QBrush(QColor("#168aad")))
                painter.drawEllipse(screen, 7.5, 7.5)
            else:
                painter.setPen(QPen(background, 0.8))
                painter.setBrush(QBrush(QColor("#168aad")))
                painter.drawEllipse(screen, 4.0, 4.0)

        for x, y, point in self._events:
            screen = self._to_screen(x, y)
            color = QColor("#d62828" if point.severity == "critical"
                           else "#f08c00")
            size = 8.0 if point.event_index == self._selected_event else 6.0
            triangle = QPainterPath(QPointF(screen.x(), screen.y() - size))
            triangle.lineTo(screen.x() - size, screen.y() + size)
            triangle.lineTo(screen.x() + size, screen.y() + size)
            triangle.closeSubpath()
            painter.setPen(QPen(background, 1))
            painter.setBrush(QBrush(color))
            painter.drawPath(triangle)

        self._draw_legend(painter, foreground, background)
        self._draw_scale_bar(painter, foreground)

    def _draw_grid(self, painter: QPainter, grid: QColor, muted: QColor) -> None:
        step = _nice_distance(90.0 / self._scale)
        left, top = self._to_world(QPointF(0, 0))
        right, bottom = self._to_world(QPointF(self.width(), self.height()))
        x = math.floor(left / step) * step
        painter.setPen(QPen(grid, 1))
        while x <= right:
            screen_x = self._to_screen(x, 0).x()
            painter.drawLine(QPointF(screen_x, 0),
                             QPointF(screen_x, self.height()))
            x += step
        y = math.floor(bottom / step) * step
        while y <= top:
            screen_y = self._to_screen(0, y).y()
            painter.drawLine(QPointF(0, screen_y),
                             QPointF(self.width(), screen_y))
            y += step
        painter.setPen(muted)
        painter.drawText(12, 22, "N ↑")

    def _draw_legend(self, painter: QPainter, foreground: QColor,
                     background: QColor) -> None:
        labels = ((QColor("#97a3ad"), "GPS fixes"),
                  (QColor("#168aad"), "Reflectance"),
                  (QColor("#d62828"), "Critical event"),
                  (QColor("#f08c00"), "Warning"))
        x, y = 54.0, 18.0
        for color, label in labels:
            painter.setPen(QPen(background, 0.8))
            painter.setBrush(color)
            painter.drawEllipse(QPointF(x, y), 4, 4)
            painter.setPen(foreground)
            painter.drawText(QPointF(x + 9, y + 4), label)
            x += painter.fontMetrics().horizontalAdvance(label) + 35

    def _draw_scale_bar(self, painter: QPainter, foreground: QColor) -> None:
        distance = _nice_distance(120.0 / self._scale)
        pixels = distance * self._scale
        x, y = 18.0, self.height() - 24.0
        painter.setPen(QPen(foreground, 2))
        painter.drawLine(QPointF(x, y), QPointF(x + pixels, y))
        painter.drawLine(QPointF(x, y - 4), QPointF(x, y + 4))
        painter.drawLine(QPointF(x + pixels, y - 4),
                         QPointF(x + pixels, y + 4))
        label = (f"{distance / 1000:g} km" if distance >= 1000
                 else f"{distance:g} m")
        painter.drawText(QPointF(x, y - 7), label)


class MissionMapPanel(QWidget):
    """Baidu Map with the local metric renderer retained as a safe fallback."""

    measurement_selected = pyqtSignal(int)
    event_selected = pyqtSignal(int)
    status_changed = pyqtSignal(str)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self._stack = QStackedWidget()
        layout.addWidget(self._stack)
        self._offline = MissionMap()
        self._offline.measurement_selected.connect(
            self.measurement_selected.emit)
        self._offline.event_selected.connect(self.event_selected.emit)
        self._stack.addWidget(self._offline)
        self._web: BaiduMissionMap | None = None
        self._web_failed = False
        self._model: MissionMapModel | None = None
        self._external_data_enabled = False
        self.status_text = "Offline metric map"
        try:
            self._web = BaiduMissionMap()
        except (CredentialError, OSError, RuntimeError) as exc:
            self.status_text = f"Offline map · {exc}"
        else:
            self._web.measurement_selected.connect(
                self.measurement_selected.emit)
            self._web.event_selected.connect(self.event_selected.emit)
            self._web.map_ready.connect(self._web_ready)
            self._web.map_error.connect(self._web_error)
            self._stack.addWidget(self._web)
            self._stack.setCurrentWidget(self._web)
            self.status_text = "Loading Baidu Map…"

    def _web_ready(self) -> None:
        self._web_failed = False
        if self._external_data_enabled:
            self.status_text = (
                "Baidu satellite · WGS84 pass-through (BD09 conversion disabled)")
        elif self._model is not None:
            self.status_text = (
                "Offline metric map · external mission data disabled")
        else:
            self.status_text = "Baidu Map ready"
        self.status_changed.emit(self.status_text)

    def _web_error(self, message: str) -> None:
        self._web_failed = True
        self._stack.setCurrentWidget(self._offline)
        self.status_text = f"Offline map fallback · {message}"
        self.status_changed.emit(self.status_text)

    @property
    def baidu_available(self) -> bool:
        return self._web is not None and not self._web_failed

    def set_model(self, model: MissionMapModel) -> None:
        self._model = model
        self._external_data_enabled = False
        self._offline.set_model(model)
        if self._web is None or self._web_failed:
            self._stack.setCurrentWidget(self._offline)
        else:
            # Keep the base map visible, but do not place recorded mission
            # coordinates into a remotely scripted page until the user agrees.
            self._stack.setCurrentWidget(self._web)
            self._web.clear_mission()
            self.status_text = "Baidu Map · mission overlay awaiting permission"
            self.status_changed.emit(self.status_text)

    def set_timezone(self, name: str, offset_minutes: int) -> None:
        self._offline.set_timezone(name, offset_minutes)

    def set_external_data_enabled(self, enabled: bool) -> None:
        self._external_data_enabled = bool(enabled and self._web is not None)
        if self._external_data_enabled and self._web is not None:
            if self._model is not None:
                self._web.set_model(self._model)
            self._stack.setCurrentWidget(self._web)
            self.status_text = (
                "Baidu satellite · WGS84 pass-through (BD09 conversion disabled)")
        else:
            if self._web is not None:
                self._web.clear_mission()
            self._stack.setCurrentWidget(self._offline)
            self.status_text = "Offline metric map · external mission data disabled"
        self.status_changed.emit(self.status_text)

    def fit_route(self) -> None:
        current = self._stack.currentWidget()
        if hasattr(current, "fit_route"):
            current.fit_route()

    def select_measurement(self, reflectance_index: int,
                           *, center: bool = False) -> None:
        self._offline.select_measurement(reflectance_index, center=center)
        if self._external_data_enabled and self._web is not None:
            self._web.select_measurement(reflectance_index, center=center)

    def focus_event(self, event_index: int) -> None:
        self._offline.focus_event(event_index)
        if self._external_data_enabled and self._web is not None:
            self._web.focus_event(event_index)


class MissionViewer(QMainWindow):
    def __init__(self, service: MissionService, *, api_port: int | None = 8765,
                 verify_crc: bool = True):
        super().__init__()
        self.service = service
        self.verify_crc = verify_crc
        self.api_server: MissionHttpServer | None = None
        self.map_model = MissionMapModel((), (), (), 0, 0)
        self.setWindowTitle("DJI H1 Mission Viewer")
        self.resize(1280, 840)
        self.setMinimumSize(900, 620)
        self._build()
        self._start_api(api_port)
        if self.service.mission is not None:
            self._refresh()

    def _build(self) -> None:
        central = QWidget()
        outer = QVBoxLayout(central)
        outer.setContentsMargins(10, 10, 10, 10)
        outer.setSpacing(8)

        toolbar = QHBoxLayout()
        open_button = QPushButton("Open mission folder…")
        open_button.clicked.connect(self.open_dialog)
        toolbar.addWidget(open_button)
        self.path_text = QLineEdit()
        self.path_text.setReadOnly(True)
        self.path_text.setPlaceholderText("No mission loaded")
        toolbar.addWidget(self.path_text, 1)
        outer.addLayout(toolbar)

        vertical = QSplitter(Qt.Orientation.Vertical)
        upper = QSplitter(Qt.Orientation.Horizontal)
        map_group = QGroupBox("Flight route")
        map_layout = QVBoxLayout(map_group)
        self.route_map = MissionMapPanel()
        self.route_map.measurement_selected.connect(self.show_measurement)
        self.route_map.event_selected.connect(self.show_event)
        map_layout.addWidget(self.route_map)
        map_controls = QHBoxLayout()
        self.map_status = QLabel(self.route_map.status_text)
        self.map_status.setStyleSheet("color: palette(mid);")
        self.route_map.status_changed.connect(self.map_status.setText)
        map_controls.addWidget(self.map_status, 1)
        fit_button = QPushButton("Fit route")
        fit_button.clicked.connect(self.route_map.fit_route)
        map_controls.addWidget(fit_button)
        map_layout.addLayout(map_controls)
        upper.addWidget(map_group)
        upper.addWidget(self._build_information_panel())
        upper.setStretchFactor(0, 3)
        upper.setStretchFactor(1, 1)

        spectrum_group = QGroupBox("Measurement result")
        spectrum_layout = QVBoxLayout(spectrum_group)
        self.spectrum_info = QLabel(
            "Select a blue measurement point on the route")
        self.spectrum_info.setWordWrap(True)
        self.spectrum_info.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse)
        spectrum_layout.addWidget(self.spectrum_info)
        self.spectrum_plot = SpectrumPlot()
        spectrum_layout.addWidget(self.spectrum_plot, 1)
        vertical.addWidget(upper)
        vertical.addWidget(spectrum_group)
        vertical.setStretchFactor(0, 3)
        vertical.setStretchFactor(1, 2)
        outer.addWidget(vertical, 1)
        self.setCentralWidget(central)
        self.setStatusBar(QStatusBar())

    def _build_information_panel(self) -> QWidget:
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        container = QWidget()
        layout = QVBoxLayout(container)
        info_group = QGroupBox("Flight information")
        info_layout = QVBoxLayout(info_group)
        self.flight_info = QLabel("No mission loaded")
        self.flight_info.setWordWrap(True)
        self.flight_info.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse)
        info_layout.addWidget(self.flight_info)
        layout.addWidget(info_group)

        events_group = QGroupBox("Critical events")
        events_layout = QVBoxLayout(events_group)
        self.events_list = QListWidget()
        self.events_list.setWordWrap(True)
        self.events_list.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.events_list.itemClicked.connect(self._event_item_clicked)
        events_layout.addWidget(self.events_list)
        self.event_details = QLabel("No located abnormal events")
        self.event_details.setWordWrap(True)
        self.event_details.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse)
        events_layout.addWidget(self.event_details)
        layout.addWidget(events_group, 1)
        scroll.setWidget(container)
        scroll.setMinimumWidth(300)
        return scroll

    def _start_api(self, api_port: int | None) -> None:
        if api_port is None:
            self.statusBar().showMessage("Read-only API disabled")
            return
        try:
            self.api_server, _thread = start_http_api(self.service,
                                                      port=api_port)
            actual_port = self.api_server.server_address[1]
            self.statusBar().showMessage(
                f"Read-only API: http://127.0.0.1:{actual_port}/api/v1")
        except OSError as exc:
            self.statusBar().showMessage(f"API unavailable: {exc}")

    def open_dialog(self) -> None:
        initial = (str(self.service.mission.path) if self.service.mission
                   else str(Path.home()))
        selected = QFileDialog.getExistingDirectory(
            self, "Select DJI H1 mission folder", initial)
        if selected:
            self.load(selected)

    def load(self, path: str | Path) -> None:
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            self.service.load(path, verify_crc=self.verify_crc)
            self._refresh()
        except Exception as exc:
            QMessageBox.critical(self, "Cannot open mission", str(exc))
        finally:
            QApplication.restoreOverrideCursor()

    def _refresh(self) -> None:
        mission = self.service.mission
        if mission is None:
            return
        self.path_text.setText(str(mission.path))
        self.map_model = build_mission_map(mission)
        self.route_map.set_timezone(mission.timezone_name,
                                    mission.timezone_offset_minutes)
        self.route_map.set_model(self.map_model)
        if self.route_map.baidu_available:
            choice = QMessageBox.question(
                self, "Display mission on Baidu Map?",
                "Baidu's remotely loaded JavaScript and map services will see "
                "the viewed geographic area, and the mission route, measurement "
                "positions, and located events will be placed into that page.\n\n"
                "Use the Baidu map for this mission?",
                QMessageBox.StandardButton.Yes |
                QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No)
            self.route_map.set_external_data_enabled(
                choice == QMessageBox.StandardButton.Yes)
        self._show_flight_info()
        self._populate_events()
        if self.map_model.measurements:
            self.show_measurement(
                self.map_model.measurements[0].reflectance_index)
        else:
            self.spectrum_info.setText("No located reflectance measurements")
            self.spectrum_plot.clear("No reflectance records")

    def _show_flight_info(self) -> None:
        mission = self.service.mission
        if mission is None:
            return
        summary = mission.summary
        overview = mission.overview()
        files = overview["files"]
        start = int(summary.get("started_utc_ms", 0) or 0)
        end = int(summary.get("updated_utc_ms", 0) or 0)
        timezone_name = mission.timezone_name
        timezone_offset = mission.timezone_offset_minutes
        duration = (max(0.0, (end - start) / 1000.0)
                    if start and end else max(
                        item["duration_seconds"] for item in files.values()))
        product_errors = overview["product_errors"]
        binary_errors = [item for item in product_errors
                         if item["filename"].upper().endswith(".BIN")]
        fatal_binary_errors = [item for item in binary_errors
                               if item.get("fatal", True)]
        recovered_binary = any(item["recovered_prefix"]
                               for item in files.values())
        all_present_crc_checked = all(
            not item["present"] or item["crc_verified"]
            for item in files.values())
        crc_text = ("failed" if fatal_binary_errors else
                    ("recovered verified prefix" if all_present_crc_checked
                     else "recovered prefix (CRC skipped)")
                    if recovered_binary else
                    "passed" if all(not item["present"] or
                                    item["crc_verified"]
                                    for item in files.values()) else
                    "not verified")
        def format_product_error(item: dict) -> str:
            text = f"{item['filename']}: {item['message']}"
            if item.get("file_offset") is not None and not item.get(
                    "fatal", True):
                text += (f"; recovered {item['recovered_records']} record(s), "
                         f"discarded {item['discarded_tail_bytes']} tail byte(s)")
            return text

        product_error_text = ("none" if not product_errors else "; ".join(
            format_product_error(item) for item in product_errors))
        rows = (
            ("Mission", summary.get("directory", mission.path.name)),
            ("Summary source", overview.get("summary_source") or "none"),
            ("State", summary.get("state", "unknown")),
            ("Drone", summary.get("drone_serial", "unknown")),
            ("Canonical drone ID",
             summary.get("drone_serial_hex", "unknown")),
            ("Firmware", summary.get("firmware_version", "unknown")),
            ("Timezone", f"{timezone_name} ({_offset_text(timezone_offset)})"),
            ("Started", _time_text(start, timezone_offset, timezone_name)),
            ("Duration", f"{duration:.1f} s"),
            ("Segments", summary.get("segments_completed",
                                     len(overview["sessions"]))),
            ("Raw spectra", files["raw"]["records"]),
            ("Reflectance", files["reflectance"]["records"]),
            ("GPS points", files["gps"]["records"]),
            ("Valid map fixes", len(self.map_model.route)),
            ("Located measurements", len(self.map_model.measurements)),
            ("Unlocated measurements", self.map_model.unlocated_measurements),
            ("Critical/warning events", len(self.map_model.events)),
            ("Unlocated abnormal events", self.map_model.unlocated_events),
            ("CRC verification", crc_text),
            ("Damaged data files", product_error_text),
        )
        self.flight_info.setText("<table>" + "".join(
            f"<tr><td><b>{html.escape(str(label))}</b></td>"
            f"<td>&nbsp;{html.escape(str(value))}</td></tr>"
            for label, value in rows) + "</table>")

    def _populate_events(self) -> None:
        self.events_list.clear()
        for point in self.map_model.events:
            label = ("CRITICAL" if point.severity == "critical" else "WARNING")
            item = QListWidgetItem(
                f"{label} · {point.event_name.replace('_', ' ')}")
            item.setData(Qt.ItemDataRole.UserRole, point.event_index)
            item.setForeground(QColor("#d62828" if point.severity == "critical"
                                      else "#c66a00"))
            self.events_list.addItem(item)
        if not self.map_model.events:
            self.event_details.setText("No located abnormal events")
        elif self.map_model.unlocated_events:
            self.event_details.setText(
                f"{self.map_model.unlocated_events} abnormal event(s) could not "
                "be placed because the GPS track did not bracket their time.")
        else:
            self.event_details.setText(
                "Select an event to center it on the route.")

    def _event_item_clicked(self, item: QListWidgetItem) -> None:
        value = item.data(Qt.ItemDataRole.UserRole)
        if value is not None:
            self.show_event(int(value))

    def show_event(self, event_index: int) -> None:
        mission = self.service.mission
        if mission is None or not (0 <= event_index < len(mission.events)):
            return
        self.route_map.focus_event(event_index)
        event = mission.events[event_index]
        details = " · ".join(
            f"{key}={value}" for key, value in event.items()
            if key not in ("schema_version", "event"))
        name = str(event.get("event", "event")).replace("_", " ")
        self.event_details.setText(
            f"<b>{html.escape(name)}</b><br>{html.escape(details)}")
        for row in range(self.events_list.count()):
            item = self.events_list.item(row)
            if item.data(Qt.ItemDataRole.UserRole) == event_index:
                self.events_list.setCurrentItem(item)
                break

    def show_measurement(self, reflectance_index: int) -> None:
        mission = self.service.mission
        if mission is None:
            return
        try:
            located = mission.located_reflectance_spectrum(reflectance_index)
        except (IndexError, FileNotFoundError, ValueError) as exc:
            self.spectrum_plot.clear("Unable to decode reflectance spectrum")
            self.spectrum_info.setText(str(exc))
            return
        self.route_map.select_measurement(reflectance_index)
        record = located.spectrum
        values = [value / 100.0
                  for value in record.reflectance_0p01_percent]
        self.spectrum_plot.set_values(values)
        info = record.info
        if located.position is None:
            position_text = "position unavailable"
        else:
            point = located.position
            altitude_text = (f"altitude {point.altitude_relative_m:.2f} m"
                             if point.altitude_relative_m is not None else
                             "altitude unavailable")
            position_text = (
                f"{point.latitude_deg:.7f}, {point.longitude_deg:.7f} · "
                f"{altitude_text} · "
                f"{point.quality}, GPS gap {point.gap_ms:.1f} ms · "
                f"{_time_domain_text(point.time_domain, point.sync_generation)}")
        self.spectrum_info.setText(
            f"<b>Reflectance calculation {info.calculation_count}</b> · "
            f"session {record.header.session_id}, "
            f"segment {record.header.segment_id} · "
            f"ground frame {info.ground_frame_count}, "
            f"sky frame {info.sky_frame_count} · "
            f"valid {info.valid_sample_count}/{info.sample_count} · "
            f"{html.escape(_time_text(record.header.utc_ms, mission.timezone_offset_minutes, mission.timezone_name))}<br>"
            f"{html.escape(position_text)}")

    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802
        if self.api_server is not None:
            self.api_server.shutdown()
            self.api_server.server_close()
        event.accept()


def run_desktop(*, initial_path: str | Path | None = None,
                api_port: int | None = 8765, verify_crc: bool = True) -> None:
    app = QApplication.instance()
    owns_application = app is None
    if app is None:
        app = QApplication(sys.argv[:1])
        app.setApplicationName("DJI H1 Mission Viewer")
        app.setStyle("Fusion")
        _configure_application_font(app)
    service = MissionService()
    if initial_path is not None:
        service.load(initial_path, verify_crc=verify_crc)
    window = MissionViewer(service, api_port=api_port,
                           verify_crc=verify_crc)
    window.show()
    if owns_application:
        app.exec()
