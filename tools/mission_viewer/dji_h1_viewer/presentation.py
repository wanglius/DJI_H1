"""UI-neutral preparation of route, measurement, and event map layers."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .decoder import GpsSample, ReflectanceRecordInfo
from .geolocation import DRONE_VALID_POSITION
from .mission import Mission


@dataclass(frozen=True)
class RoutePoint:
    latitude_deg: float
    longitude_deg: float
    altitude_relative_m: float
    b_monotonic_us: int
    gps_index: int


@dataclass(frozen=True)
class MeasurementPoint:
    latitude_deg: float
    longitude_deg: float
    altitude_relative_m: float
    reflectance_index: int
    session_id: int
    segment_id: int
    calculation_count: int
    utc_ms: int
    quality: str
    gps_gap_ms: float
    time_domain: str
    sync_generation: int | None


@dataclass(frozen=True)
class MissionEventPoint:
    latitude_deg: float
    longitude_deg: float
    event_index: int
    event_name: str
    severity: str
    b_monotonic_us: int
    utc_ms: int


@dataclass(frozen=True)
class MissionMapModel:
    route: tuple[RoutePoint, ...]
    measurements: tuple[MeasurementPoint, ...]
    events: tuple[MissionEventPoint, ...]
    unlocated_measurements: int
    unlocated_events: int


_CRITICAL_NAMES = {
    "protocol_crc_error", "protocol_timeout", "storage_error",
    "write_error", "flush_error", "acquisition_error", "rx_overrun",
    "drone_identity_mismatch",
}
_WARNING_NAMES = {
    "clock_observation_drop", "reflectance_rejected", "frame_drop",
}


def event_severity(event: dict[str, Any]) -> str | None:
    """Classify map-worthy abnormal events without treating normal lifecycle
    transitions as alarms.

    Explicit severity supplied by a future recorder takes priority. The v01
    stop reason is also decoded so low battery, manual abort, and drone-link
    loss from the A-B protocol appear on the route.
    """

    explicit = str(event.get("severity", "")).lower()
    if explicit in ("critical", "error"):
        return "critical"
    if explicit in ("warning", "warn"):
        return "warning"
    name = str(event.get("event", "")).lower()
    if name in _CRITICAL_NAMES or any(
            token in name for token in ("failure", "failed", "corrupt",
                                        "timeout", "overrun")):
        return "critical"
    if name in _WARNING_NAMES or "rejected" in name or "drop" in name:
        return "warning"
    if name == "capture_result":
        try:
            return "critical" if int(event.get("argument1", 0)) != 0 else None
        except (TypeError, ValueError):
            return "critical"
    if name == "stop_request":
        try:
            reason = int(event.get("argument1", 0))
        except (TypeError, ValueError):
            return None
        if reason in (3, 6, 7):  # low battery, manual abort, aircraft link loss
            return "critical"
        if reason in (2, 4):  # return-home or automatic landing
            return "warning"
    return None


def build_mission_map(mission: Mission) -> MissionMapModel:
    """Build compact map layers without loading any spectrum sample arrays."""

    route: list[RoutePoint] = []
    if mission.gps is not None:
        for index, ref in enumerate(mission.gps.records):
            sample = ref.info
            if not isinstance(sample, GpsSample) or not (
                    sample.valid_flags & DRONE_VALID_POSITION):
                continue
            if not (-900_000_000 <= sample.latitude_e7 <= 900_000_000 and
                    -1_800_000_000 <= sample.longitude_e7 <= 1_800_000_000):
                continue
            route.append(RoutePoint(
                sample.latitude_e7 / 1e7, sample.longitude_e7 / 1e7,
                sample.altitude_relative_mm / 1000.0,
                ref.header.b_monotonic_us, index))

    measurements: list[MeasurementPoint] = []
    unlocated_measurements = 0
    if mission.reflectance is not None:
        for index, ref in enumerate(mission.reflectance.records):
            info = ref.info
            if not isinstance(info, ReflectanceRecordInfo):
                continue
            position = mission.position_for_header(ref.header)
            if position is None:
                unlocated_measurements += 1
                continue
            measurements.append(MeasurementPoint(
                position.latitude_deg, position.longitude_deg,
                position.altitude_relative_m, index, ref.header.session_id,
                ref.header.segment_id, info.calculation_count,
                ref.header.utc_ms, position.quality, position.gap_ms,
                position.time_domain, position.sync_generation))

    events: list[MissionEventPoint] = []
    unlocated_events = 0
    for index, event in enumerate(mission.events):
        severity = event_severity(event)
        if severity is None:
            continue
        try:
            b_monotonic_us = int(event.get("b_monotonic_us", -1))
        except (TypeError, ValueError):
            b_monotonic_us = -1
        if b_monotonic_us < 0:
            unlocated_events += 1
            continue
        position = mission.position_at_b_monotonic_us(b_monotonic_us)
        if position is None:
            unlocated_events += 1
            continue
        try:
            utc_ms = int(event.get("utc_ms", 0))
        except (TypeError, ValueError):
            utc_ms = 0
        events.append(MissionEventPoint(
            position.latitude_deg, position.longitude_deg, index,
            str(event.get("event", "unknown")), severity,
            b_monotonic_us, utc_ms))

    return MissionMapModel(tuple(route), tuple(measurements), tuple(events),
                           unlocated_measurements, unlocated_events)
