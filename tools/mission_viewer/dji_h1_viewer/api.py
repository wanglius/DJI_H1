"""Public query service and optional read-only localhost HTTP API."""

from __future__ import annotations

from dataclasses import asdict
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
from threading import RLock, Thread
from typing import Any
from urllib.parse import parse_qs, urlparse

from .decoder import RecordFormatError
from .geolocation import TimeDomain
from .mission import GROUND, SKY, Mission, open_mission
from .presentation import build_mission_map


class MissionService:
    """Thread-safe facade shared by the desktop UI and external callers.

    Consumers may import this class directly. The HTTP layer below is only a
    thin adapter, so imported and HTTP clients see the same decoded results.
    """

    def __init__(self, mission: Mission | None = None):
        self._lock = RLock()
        self._mission = mission

    @property
    def mission(self) -> Mission | None:
        with self._lock:
            return self._mission

    def load(self, path: str | Path, *, verify_crc: bool = True) -> dict[str, Any]:
        # Decode first so a bad selection does not discard the current mission.
        candidate = open_mission(path, verify_crc=verify_crc)
        with self._lock:
            self._mission = candidate
            return candidate.overview()

    def _require_mission(self) -> Mission:
        mission = self.mission
        if mission is None:
            raise RuntimeError("no mission is loaded")
        return mission

    def overview(self) -> dict[str, Any]:
        return self._require_mission().overview()

    def raw_index(self, *, offset: int = 0, limit: int = 500,
                  role: int | None = None, session_id: int | None = None,
                  segment_id: int | None = None) -> dict[str, Any]:
        mission = self._require_mission()
        indices = mission.raw_indices(role=role, session_id=session_id,
                                      segment_id=segment_id)
        offset, limit = _page_bounds(offset, limit)
        page = []
        if mission.raw:
            for index in indices[offset:offset + limit]:
                ref = mission.raw.records[index]
                page.append({"index": index, "header": asdict(ref.header),
                             "info": asdict(ref.info)})
        return {"total": len(indices), "offset": offset, "items": page}

    def raw_spectrum(self, index: int, *, time_domain: TimeDomain = "auto",
                     max_gap_ms: float | None = None) -> dict[str, Any]:
        mission = self._require_mission()
        located = mission.located_raw_spectrum(
            index, time_domain=time_domain, max_gap_ms=max_gap_ms)
        record = located.spectrum
        return {"index": index, "header": asdict(record.header),
                "info": asdict(record.info), "samples": list(record.samples),
                "position": asdict(located.position) if located.position else None}

    def reflectance_index(self, *, offset: int = 0, limit: int = 500,
                          session_id: int | None = None,
                          segment_id: int | None = None) -> dict[str, Any]:
        mission = self._require_mission()
        indices = mission.reflectance_indices(session_id=session_id,
                                              segment_id=segment_id)
        offset, limit = _page_bounds(offset, limit)
        page = []
        if mission.reflectance:
            for index in indices[offset:offset + limit]:
                ref = mission.reflectance.records[index]
                page.append({"index": index, "header": asdict(ref.header),
                             "info": asdict(ref.info)})
        return {"total": len(indices), "offset": offset, "items": page}

    def reflectance_spectrum(self, index: int, *, time_domain: TimeDomain = "auto",
                             max_gap_ms: float | None = None) -> dict[str, Any]:
        mission = self._require_mission()
        located = mission.located_reflectance_spectrum(
            index, time_domain=time_domain, max_gap_ms=max_gap_ms)
        record = located.spectrum
        values = list(record.reflectance_0p01_percent)
        return {
            "index": index,
            "header": asdict(record.header),
            "info": asdict(record.info),
            "reflectance_0p01_percent": values,
            "reflectance_percent": [value / 100.0 for value in values],
            "sample_flags": list(record.sample_flags),
            "position": asdict(located.position) if located.position else None,
        }

    def position_at_b_monotonic_us(
            self, b_monotonic_us: int, *, max_gap_ms: float | None = None) \
            -> dict[str, Any] | None:
        position = self._require_mission().position_at_b_monotonic_us(
            b_monotonic_us, max_gap_ms=max_gap_ms)
        return asdict(position) if position else None

    def map_data(self) -> dict[str, Any]:
        """Return route, located measurements, and abnormal event overlays."""

        return asdict(build_mission_map(self._require_mission()))

    def gps(self, *, offset: int = 0, limit: int = 500) -> dict[str, Any]:
        mission = self._require_mission()
        offset, limit = _page_bounds(offset, limit)
        total = len(mission.gps) if mission.gps else 0
        return {"total": total, "offset": offset,
                "items": mission.gps_page(offset, limit)}

    def events(self, *, offset: int = 0, limit: int = 500) -> dict[str, Any]:
        mission = self._require_mission()
        offset, limit = _page_bounds(offset, limit)
        return {"total": len(mission.events), "offset": offset,
                "items": mission.events[offset:offset + limit],
                "read_errors": [asdict(item) for item in mission.event_errors]}


def _page_bounds(offset: int, limit: int) -> tuple[int, int]:
    return max(0, offset), max(0, min(limit, 5000))


class MissionHttpServer(ThreadingHTTPServer):
    """HTTP server carrying the service instance used by request handlers."""

    daemon_threads = True

    def __init__(self, address: tuple[str, int], service: MissionService):
        self.service = service
        super().__init__(address, _RequestHandler)


class _RequestHandler(BaseHTTPRequestHandler):
    server: MissionHttpServer

    def do_GET(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        try:
            payload = self._route(parsed.path, query)
            self._json(HTTPStatus.OK, payload)
        except RecordFormatError as exc:
            self._json(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": str(exc)})
        except (ValueError, IndexError) as exc:
            self._json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
        except (RuntimeError, FileNotFoundError) as exc:
            self._json(HTTPStatus.NOT_FOUND, {"error": str(exc)})
        except Exception as exc:  # Keep the background API thread alive.
            self._json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})

    def _route(self, path: str, query: dict[str, list[str]]) -> dict[str, Any]:
        service = self.server.service
        if path == "/api/v1/health":
            return {"ok": True, "mission_loaded": service.mission is not None}
        if path == "/api/v1/mission":
            return service.overview()
        if path == "/api/v1/raw-index":
            role_text = _one(query, "role")
            role = {None: None, "ground": GROUND, "sky": SKY}.get(role_text)
            if role_text not in (None, "ground", "sky"):
                raise ValueError("role must be ground or sky")
            return service.raw_index(
                offset=_integer(query, "offset", 0),
                limit=_integer(query, "limit", 500), role=role,
                session_id=_optional_integer(query, "session_id"),
                segment_id=_optional_integer(query, "segment_id"))
        if path == "/api/v1/raw":
            return service.raw_spectrum(
                _integer(query, "index"),
                time_domain=_one(query, "time_domain") or "auto",
                max_gap_ms=_optional_float(query, "max_gap_ms"))
        if path == "/api/v1/reflectance-index":
            return service.reflectance_index(
                offset=_integer(query, "offset", 0),
                limit=_integer(query, "limit", 500),
                session_id=_optional_integer(query, "session_id"),
                segment_id=_optional_integer(query, "segment_id"))
        if path == "/api/v1/reflectance":
            return service.reflectance_spectrum(
                _integer(query, "index"),
                time_domain=_one(query, "time_domain") or "auto",
                max_gap_ms=_optional_float(query, "max_gap_ms"))
        if path == "/api/v1/position":
            return {"position": service.position_at_b_monotonic_us(
                _integer(query, "b_monotonic_us"),
                max_gap_ms=_optional_float(query, "max_gap_ms"))}
        if path == "/api/v1/map":
            return service.map_data()
        if path == "/api/v1/gps":
            return service.gps(offset=_integer(query, "offset", 0),
                               limit=_integer(query, "limit", 500))
        if path == "/api/v1/events":
            return service.events(offset=_integer(query, "offset", 0),
                                  limit=_integer(query, "limit", 500))
        raise FileNotFoundError(f"unknown API route: {path}")

    def _json(self, status: HTTPStatus, payload: Any) -> None:
        encoded = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status.value)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, format: str, *args: object) -> None:
        # The desktop status bar is the user-facing API indicator. Suppress the
        # default request-by-request stderr noise.
        del format, args


def _one(query: dict[str, list[str]], name: str) -> str | None:
    values = query.get(name)
    return values[0] if values else None


def _integer(query: dict[str, list[str]], name: str,
             default: int | None = None) -> int:
    value = _one(query, name)
    if value is None:
        if default is None:
            raise ValueError(f"missing query parameter: {name}")
        return default
    try:
        return int(value)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc


def _optional_integer(query: dict[str, list[str]], name: str) -> int | None:
    return _integer(query, name) if _one(query, name) is not None else None


def _optional_float(query: dict[str, list[str]], name: str) -> float | None:
    value = _one(query, name)
    if value is None:
        return None
    try:
        return float(value)
    except ValueError as exc:
        raise ValueError(f"{name} must be a number") from exc


def start_http_api(service: MissionService, *, host: str = "127.0.0.1",
                   port: int = 8765) -> tuple[MissionHttpServer, Thread]:
    """Start the read-only API in a daemon thread and return server/thread.

    The default loopback binding deliberately prevents remote network access.
    Call ``server.shutdown()`` during orderly shutdown.
    """

    server = MissionHttpServer((host, port), service)
    thread = Thread(target=server.serve_forever, name="mission-http-api",
                    daemon=True)
    thread.start()
    return server, thread
