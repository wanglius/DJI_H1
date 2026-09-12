"""Timestamp-aware interpolation of spectrum positions from GPS records."""

from __future__ import annotations

from bisect import bisect_left
from dataclasses import dataclass
import math
from typing import Literal, Sequence

from .decoder import GpsSample, RecordHeader, RecordRef


RECORD_TIME_VALID_A_MONOTONIC = 1 << 1
DRONE_VALID_POSITION = 1 << 0
DRONE_VALID_ALTITUDE = 1 << 1
DEFAULT_WIDE_GAP_MS = 500.0
_U32_MODULUS = 1 << 32
_U32_HALF_RANGE = 1 << 31

TimeDomain = Literal["auto", "a_monotonic_ms", "b_monotonic_us"]


@dataclass(frozen=True)
class InterpolatedPosition:
    """A position estimate plus enough provenance to audit it later."""

    latitude_deg: float
    longitude_deg: float
    altitude_relative_m: float | None
    time_domain: str
    target_time: int
    before_time: int
    after_time: int
    before_gps_index: int
    after_gps_index: int
    interpolation_fraction: float
    gap_ms: float
    quality: str
    before_protocol_sequence: int
    after_protocol_sequence: int
    before_gps_fix: int
    after_gps_fix: int
    before_rtk_solution: int
    after_rtk_solution: int
    sync_generation: int | None


@dataclass(frozen=True)
class _PositionPoint:
    time: int
    gps_index: int
    sample: GpsSample


def _extend_u32_near(raw_value: int, reference: int) -> int:
    """Lift a uint32 timestamp to the epoch nearest a correlated 64-bit time."""

    candidate = (reference & ~0xFFFFFFFF) | raw_value
    if candidate - reference > _U32_HALF_RANGE:
        candidate -= _U32_MODULUS
    elif reference - candidate > _U32_HALF_RANGE:
        candidate += _U32_MODULUS
    return candidate


def _longitude_lerp(left: float, right: float, fraction: float) -> float:
    # Follow the short arc if a mission crosses the ±180-degree meridian.
    delta = right - left
    if delta > 180.0:
        delta -= 360.0
    elif delta < -180.0:
        delta += 360.0
    value = left + delta * fraction
    return (value + 180.0) % 360.0 - 180.0


class PositionInterpolator:
    """Reusable GPS index supporting O(log n) position lookup.

    A-monotonic interpolation is preferred because the GPS body timestamp
    describes when A produced the navigation sample, while its B timestamp is
    the slightly later UART receive-completion time. B-monotonic interpolation
    remains a fallback for records created before clock correlation is valid.
    """

    def __init__(self, gps_records: Sequence[RecordRef]):
        b_points: list[_PositionPoint] = []
        a_points: dict[int, list[_PositionPoint]] = {}
        for gps_index, ref in enumerate(gps_records):
            sample = ref.info
            if not isinstance(sample, GpsSample) or not (
                    sample.valid_flags & DRONE_VALID_POSITION):
                continue
            # The validity bit is authoritative, but range-check as a second
            # line of defence against a bad upstream navigation payload.
            if not (-900_000_000 <= sample.latitude_e7 <= 900_000_000 and
                    -1_800_000_000 <= sample.longitude_e7 <= 1_800_000_000):
                continue
            b_points.append(_PositionPoint(
                ref.header.b_monotonic_us, gps_index, sample))
            if ref.header.time_valid_flags & RECORD_TIME_VALID_A_MONOTONIC:
                source_a_ms = _extend_u32_near(
                    sample.a_monotonic_ms, ref.header.a_monotonic_ms)
                a_points.setdefault(ref.header.sync_generation, []).append(
                    _PositionPoint(source_a_ms, gps_index, sample))
        self._b_points = tuple(sorted(b_points, key=lambda item: item.time))
        self._a_points = {
            generation: tuple(sorted(points, key=lambda item: item.time))
            for generation, points in a_points.items()
        }
        self._b_times = tuple(item.time for item in self._b_points)
        self._a_times = {
            generation: tuple(item.time for item in points)
            for generation, points in self._a_points.items()
        }

    def for_header(self, header: RecordHeader, *, time_domain: TimeDomain = "auto",
                   max_gap_ms: float | None = None,
                   wide_gap_ms: float = DEFAULT_WIDE_GAP_MS) \
            -> InterpolatedPosition | None:
        if time_domain not in ("auto", "a_monotonic_ms", "b_monotonic_us"):
            raise ValueError(f"unsupported time domain: {time_domain}")
        self._validate_gaps(max_gap_ms, wide_gap_ms)

        if time_domain in ("auto", "a_monotonic_ms") and (
                header.time_valid_flags & RECORD_TIME_VALID_A_MONOTONIC):
            generation = header.sync_generation
            result = self._interpolate(
                self._a_points.get(generation, ()),
                self._a_times.get(generation, ()), header.a_monotonic_ms,
                "a_monotonic_ms", 1.0, generation, max_gap_ms, wide_gap_ms)
            if result is not None or time_domain == "a_monotonic_ms":
                return result
        if time_domain in ("auto", "b_monotonic_us"):
            return self._interpolate(
                self._b_points, self._b_times, header.b_monotonic_us,
                "b_monotonic_us", 1000.0, None, max_gap_ms, wide_gap_ms)
        return None

    def at_b_monotonic_us(self, b_monotonic_us: int, *,
                          max_gap_ms: float | None = None,
                          wide_gap_ms: float = DEFAULT_WIDE_GAP_MS) \
            -> InterpolatedPosition | None:
        if b_monotonic_us < 0:
            raise ValueError("b_monotonic_us must be non-negative")
        self._validate_gaps(max_gap_ms, wide_gap_ms)
        return self._interpolate(
            self._b_points, self._b_times, b_monotonic_us,
            "b_monotonic_us", 1000.0, None, max_gap_ms, wide_gap_ms)

    @staticmethod
    def _validate_gaps(max_gap_ms: float | None, wide_gap_ms: float) -> None:
        if max_gap_ms is not None and (
                not math.isfinite(max_gap_ms) or max_gap_ms < 0):
            raise ValueError("max_gap_ms must be a finite non-negative number")
        if not math.isfinite(wide_gap_ms) or wide_gap_ms < 0:
            raise ValueError("wide_gap_ms must be a finite non-negative number")

    @staticmethod
    def _interpolate(points: Sequence[_PositionPoint], times: Sequence[int],
                     target: int, time_domain: str, units_per_ms: float,
                     generation: int | None, max_gap_ms: float | None,
                     wide_gap_ms: float) -> InterpolatedPosition | None:
        position = bisect_left(times, target)
        if position < len(times) and times[position] == target:
            before = after = points[position]
        elif position == 0 or position == len(times):
            # Do not silently extrapolate beyond the measured navigation track.
            return None
        else:
            before, after = points[position - 1], points[position]
        interval = after.time - before.time
        if interval < 0:
            return None
        gap_ms = interval / units_per_ms
        if max_gap_ms is not None and gap_ms > max_gap_ms:
            return None
        fraction = 0.0 if interval == 0 else (target - before.time) / interval
        left, right = before.sample, after.sample
        latitude = (left.latitude_e7 +
                    (right.latitude_e7 - left.latitude_e7) * fraction) / 1e7
        longitude = _longitude_lerp(left.longitude_e7 / 1e7,
                                    right.longitude_e7 / 1e7, fraction)
        # Position and relative altitude have independent protocol validity
        # bits. Never turn an invalid placeholder altitude into plausible
        # interpolated flight data.
        if ((left.valid_flags & DRONE_VALID_ALTITUDE) and
                (right.valid_flags & DRONE_VALID_ALTITUDE)):
            altitude = (left.altitude_relative_mm +
                        (right.altitude_relative_mm -
                         left.altitude_relative_mm) * fraction) / 1000.0
        else:
            altitude = None
        quality = "exact" if before.gps_index == after.gps_index else (
            "wide_gap" if gap_ms > wide_gap_ms else "interpolated")
        return InterpolatedPosition(
            latitude, longitude, altitude, time_domain, target,
            before.time, after.time, before.gps_index, after.gps_index,
            fraction, gap_ms, quality,
            left.protocol_sequence, right.protocol_sequence,
            left.gps_fix, right.gps_fix,
            left.rtk_solution, right.rtk_solution, generation)
