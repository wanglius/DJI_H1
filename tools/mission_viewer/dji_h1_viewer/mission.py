"""Mission-level public API for DJI H1 recorded data."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
import json
from pathlib import Path
from typing import Any

from .decoder import (
    GpsSample, RawRecordInfo, RawSpectrum, RecordFile, RecordFormatError,
    RecordHeader, ReflectanceRecordInfo, ReflectanceSpectrum, RECORD_GPS,
    RECORD_RAW_SPECTRUM, RECORD_REFLECTANCE, ROLE_GROUND, ROLE_SKY,
)
from .geolocation import InterpolatedPosition, PositionInterpolator, TimeDomain


@dataclass(frozen=True)
class EventReadError:
    line_number: int
    message: str


@dataclass(frozen=True)
class ProductReadError:
    filename: str
    message: str
    file_offset: int | None = None
    recovered_records: int = 0
    discarded_tail_bytes: int = 0
    fatal: bool = True


@dataclass(frozen=True)
class LocatedRawSpectrum:
    spectrum: RawSpectrum
    position: InterpolatedPosition | None


@dataclass(frozen=True)
class LocatedReflectanceSpectrum:
    spectrum: ReflectanceSpectrum
    position: InterpolatedPosition | None


@dataclass
class Mission:
    """One mission directory and its lazily decoded data products."""

    path: Path
    summary: dict[str, Any]
    summary_source: str | None
    events: list[dict[str, Any]]
    event_errors: list[EventReadError]
    product_errors: list[ProductReadError]
    raw: RecordFile | None
    reflectance: RecordFile | None
    gps: RecordFile | None
    _positions: PositionInterpolator | None = field(
        init=False, repr=False, default=None)

    def __post_init__(self) -> None:
        if self.gps is not None:
            self._positions = PositionInterpolator(self.gps.records)

    def _timezone_metadata(self) -> tuple[str, int]:
        """Return one validated name/offset pair; never mix partial metadata."""
        timezone = self.summary.get("timezone", {})
        if not isinstance(timezone, dict):
            return ("UTC", 0)
        name = timezone.get("name")
        offset = timezone.get("utc_offset_minutes")
        safe_name = (
            isinstance(name, str) and 0 < len(name) <= 63 and
            all(character.isascii() and
                (character.isalnum() or character in "/_-+.")
                for character in name)
        )
        safe_offset = (
            not isinstance(offset, bool) and isinstance(offset, int) and
            -720 <= offset <= 840
        )
        return (name, offset) if safe_name and safe_offset else ("UTC", 0)

    @property
    def timezone_offset_minutes(self) -> int:
        """Configured fixed civil-time offset, or UTC for legacy missions."""
        return self._timezone_metadata()[1]

    @property
    def timezone_name(self) -> str:
        """Human-readable timezone label, safely bounded for presentation."""
        return self._timezone_metadata()[0]

    @classmethod
    def open(cls, path: str | Path, *, verify_crc: bool = True,
             strict_products: bool = False) -> "Mission":
        root = Path(path).expanduser().resolve()
        if not root.is_dir():
            raise FileNotFoundError(f"mission directory not found: {root}")

        product_errors: list[ProductReadError] = []
        summary: dict[str, Any] = {}
        summary_source: str | None = None
        primary_error: Exception | None = None
        summary_path = root / "MISSION.JSON"
        backup_path = root / "MISSION.BAK"

        def read_summary(candidate: Path) -> dict[str, Any]:
            with candidate.open("r", encoding="utf-8") as stream:
                value = json.load(stream)
            if not isinstance(value, dict):
                raise ValueError("mission summary is not a JSON object")
            return value

        if summary_path.exists():
            try:
                summary = read_summary(summary_path)
                summary_source = summary_path.name
            except (OSError, UnicodeError, json.JSONDecodeError,
                    ValueError) as exc:
                primary_error = exc
        if summary_source is None and backup_path.exists():
            try:
                summary = read_summary(backup_path)
                summary_source = backup_path.name
            except (OSError, UnicodeError, json.JSONDecodeError,
                    ValueError) as exc:
                product_errors.append(ProductReadError(
                    backup_path.name, str(exc)))
        if primary_error is not None:
            message = str(primary_error)
            if summary_source == backup_path.name:
                message += "; recovered summary from MISSION.BAK"
            product_errors.append(ProductReadError(
                summary_path.name, message,
                fatal=summary_source is None))
        elif not summary_path.exists() and summary_source == backup_path.name:
            product_errors.append(ProductReadError(
                summary_path.name,
                "missing; recovered summary from MISSION.BAK", fatal=False))

        events: list[dict[str, Any]] = []
        event_errors: list[EventReadError] = []
        event_path = root / "EVENTS.JSONL"
        if event_path.exists():
            with event_path.open("r", encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, 1):
                    if not line.strip():
                        continue
                    try:
                        value = json.loads(line)
                        if not isinstance(value, dict):
                            raise ValueError("event is not a JSON object")
                        events.append(value)
                    except (json.JSONDecodeError, ValueError) as exc:
                        # JSONL is intentionally tail-recoverable. Preserve all
                        # earlier valid events and report damaged lines.
                        event_errors.append(EventReadError(line_number, str(exc)))

        def optional_file(name: str, expected_type: int) -> RecordFile | None:
            candidate = root / name
            if not candidate.exists():
                return None
            try:
                record_file = RecordFile.open(
                    candidate, verify_crc=verify_crc,
                    strict=strict_products)
                if record_file.header.record_type != expected_type:
                    raise RecordFormatError(
                        f"contains record type {record_file.header.record_type}, "
                        f"expected {expected_type}")
                if record_file.scan_issue is not None:
                    issue = record_file.scan_issue
                    product_errors.append(ProductReadError(
                        name, issue.message, issue.file_offset,
                        issue.recovered_records, issue.discarded_tail_bytes,
                        fatal=False))
                return record_file
            except (OSError, RecordFormatError) as exc:
                # Mission products are independent. A damaged spectrum file
                # must not hide usable GPS, events, or the other spectrum file.
                product_errors.append(ProductReadError(name, str(exc)))
                return None

        mission = cls(
            root, summary, summary_source, events, event_errors, product_errors,
            optional_file("RAW_SPECTRA.BIN", RECORD_RAW_SPECTRUM),
            optional_file("REFLECTANCE.BIN", RECORD_REFLECTANCE),
            optional_file("GPS_TRACK.BIN", RECORD_GPS),
        )
        if (mission.raw is None and mission.reflectance is None and
                mission.gps is None and not summary and not events and
                not event_errors and not product_errors):
            raise FileNotFoundError(f"no DJI H1 record files found in {root}")
        return mission

    @staticmethod
    def _duration_seconds(record_file: RecordFile | None) -> float:
        if record_file is None or len(record_file) < 2:
            return 0.0
        times = (ref.header.b_monotonic_us for ref in record_file.records)
        first = next(times)
        earliest = latest = first
        for value in times:
            earliest = min(earliest, value)
            latest = max(latest, value)
        return (latest - earliest) / 1_000_000.0

    def overview(self) -> dict[str, Any]:
        files = {}
        for key, record_file in (
            ("raw", self.raw), ("reflectance", self.reflectance),
            ("gps", self.gps)):
            files[key] = {
                "present": record_file is not None,
                "records": len(record_file) if record_file is not None else 0,
                "bytes": (record_file.path.stat().st_size
                          if record_file is not None else 0),
                "duration_seconds": self._duration_seconds(record_file),
                "crc_verified": bool(record_file is not None and
                                     record_file.crc_verified),
                "recovered_prefix": bool(record_file is not None and
                                         record_file.scan_issue is not None),
            }
        sessions = sorted({
            (ref.header.session_id, ref.header.segment_id)
            for record_file in (self.raw, self.reflectance, self.gps)
            if record_file
            for ref in record_file.records
            if ref.header.session_id or ref.header.segment_id
        })
        return {
            "path": str(self.path),
            "summary": self.summary,
            "summary_source": self.summary_source,
            "files": files,
            "event_count": len(self.events),
            "event_errors": [asdict(item) for item in self.event_errors],
            "product_errors": [asdict(item) for item in self.product_errors],
            "sessions": [
                {"session_id": session, "segment_id": segment}
                for session, segment in sessions
            ],
        }

    def raw_indices(self, *, role: int | None = None,
                    session_id: int | None = None,
                    segment_id: int | None = None) -> list[int]:
        if self.raw is None:
            return []
        result = []
        for index, ref in enumerate(self.raw.records):
            info = ref.info
            if not isinstance(info, RawRecordInfo):
                continue
            if role is not None and info.spectrometer_role != role:
                continue
            if session_id is not None and ref.header.session_id != session_id:
                continue
            if segment_id is not None and ref.header.segment_id != segment_id:
                continue
            result.append(index)
        return result

    def reflectance_indices(self, *, session_id: int | None = None,
                            segment_id: int | None = None) -> list[int]:
        if self.reflectance is None:
            return []
        return [
            index for index, ref in enumerate(self.reflectance.records)
            if isinstance(ref.info, ReflectanceRecordInfo)
            and (session_id is None or ref.header.session_id == session_id)
            and (segment_id is None or ref.header.segment_id == segment_id)
        ]

    def gps_page(self, offset: int = 0, limit: int = 500) -> list[dict[str, Any]]:
        if self.gps is None:
            return []
        offset = max(0, offset)
        limit = max(0, min(limit, 5000))
        result = []
        for ref in self.gps.records[offset:offset + limit]:
            if isinstance(ref.info, GpsSample):
                result.append({"header": asdict(ref.header),
                               "sample": asdict(ref.info)})
        return result

    def position_for_header(
            self, header: RecordHeader, *, time_domain: TimeDomain = "auto",
            max_gap_ms: float | None = None) -> InterpolatedPosition | None:
        """Interpolate the navigation position at an arbitrary record time.

        ``auto`` prefers synchronized A monotonic time and falls back to B
        monotonic time. A result is returned only when valid GPS samples bracket
        the target; use ``max_gap_ms`` to reject estimates across long outages.
        """

        if self._positions is None:
            return None
        return self._positions.for_header(
            header, time_domain=time_domain, max_gap_ms=max_gap_ms)

    def position_at_b_monotonic_us(
            self, b_monotonic_us: int, *, max_gap_ms: float | None = None) \
            -> InterpolatedPosition | None:
        """Interpolate a position for an external B-monotonic timestamp."""

        if self._positions is None:
            return None
        return self._positions.at_b_monotonic_us(
            b_monotonic_us, max_gap_ms=max_gap_ms)

    def located_raw_spectrum(
            self, index: int, *, time_domain: TimeDomain = "auto",
            max_gap_ms: float | None = None) -> LocatedRawSpectrum:
        if self.raw is None:
            raise FileNotFoundError("RAW_SPECTRA.BIN is not present")
        spectrum = self.raw.raw_spectrum(index)
        return LocatedRawSpectrum(
            spectrum, self.position_for_header(
                spectrum.header, time_domain=time_domain,
                max_gap_ms=max_gap_ms))

    def located_reflectance_spectrum(
            self, index: int, *, time_domain: TimeDomain = "auto",
            max_gap_ms: float | None = None) -> LocatedReflectanceSpectrum:
        if self.reflectance is None:
            raise FileNotFoundError("REFLECTANCE.BIN is not present")
        spectrum = self.reflectance.reflectance_spectrum(index)
        return LocatedReflectanceSpectrum(
            spectrum, self.position_for_header(
                spectrum.header, time_domain=time_domain,
                max_gap_ms=max_gap_ms))


def open_mission(path: str | Path, *, verify_crc: bool = True,
                 strict_products: bool = False) -> Mission:
    """Open a mission directory. This is the primary public API entry point."""

    return Mission.open(path, verify_crc=verify_crc,
                        strict_products=strict_products)


GROUND = ROLE_GROUND
SKY = ROLE_SKY
