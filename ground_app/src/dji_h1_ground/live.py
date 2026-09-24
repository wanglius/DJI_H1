"""Live MQTT ingestion for the two-mode DJI H1 mission viewer.

The MQTT callback only enqueues bytes. A decoder worker handles DTF2 and
records; a separate worker publishes DTA1. Qt requests mission snapshots.
"""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
from queue import Empty, Full, Queue
from threading import Event, RLock, Thread
import time
from typing import Any

from .decoder import (
    FileHeader, GpsRecord, GpsSample, OperationEvent, RawSpectrum,
    RecordFormatError, RecordRef, ReflectanceSpectrum, RECORD_GPS,
    RECORD_OPERATION_LOG, RECORD_RAW_SPECTRUM, RECORD_REFLECTANCE,
    decode_record,
)
from .mission import Mission
from .live_transport import LiveMqttTransport
from .telemetry import (
    ACK_STATUS_PERMANENT_REJECTION, FragmentError, MESSAGE_GPS,
    MESSAGE_GPS_BATCH, MESSAGE_OPERATION_LOG, MESSAGE_REFLECTANCE,
    ReassembledTelemetry,
    TelemetryFragmentStreamDecoder, TelemetryReassembler, decode_gps_batch,
    encode_acknowledgement, decode_message_envelope, MESSAGE_MAGIC_BYTES,
)


class LiveConfigError(ValueError):
    """A local live-viewer configuration is missing or unsafe."""


_EVENT_NAMES = {
    1: "handshake",
    2: "segment_start",
    3: "stop_request",
    4: "segment_end",
    5: "power_off_request",
    6: "protocol_crc_error",
    7: "protocol_timeout",
    8: "clock_observation_drop",
    9: "reflectance_rejected",
    10: "capture_result",
    11: "flight_closed",
    12: "drone_identity_mismatch",
    13: "ab_link_lost",
    14: "ab_link_restored",
}
_EVENT_SEVERITIES = {
    0: "info",
    1: "warning",
    2: "error",
    3: "critical",
}


def _event_dictionary(event: OperationEvent) -> dict[str, Any]:
    """Project a validated compact event into the offline JSONL schema."""

    header = event.header
    info = event.info
    return {
        "schema_version": 1,
        "sequence": header.sequence,
        "event": _EVENT_NAMES.get(info.event_code,
                                  f"event_{info.event_code}"),
        "event_code": info.event_code,
        "severity": _EVENT_SEVERITIES[info.severity],
        "session_id": header.session_id,
        "segment_id": header.segment_id,
        "b_monotonic_us": header.b_monotonic_us,
        "utc_ms": header.utc_ms,
        "sync_state": header.sync_state,
        "time_valid_flags": header.time_valid_flags,
        "argument0": info.argument0,
        "argument1": info.argument1,
    }


def _integer(value: Any, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise LiveConfigError(f"{name} must be an integer")
    try:
        parsed = int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as exc:
        raise LiveConfigError(f"{name} must be an integer") from exc
    if not minimum <= parsed <= maximum:
        raise LiveConfigError(f"{name} must be in {minimum}..{maximum}")
    return parsed


def _text(value: Any, name: str, *, required: bool = True,
          maximum: int = 255, strip: bool = True) -> str:
    if value is None and not required:
        return ""
    candidate = value.strip() if isinstance(value, str) and strip else value
    if not isinstance(candidate, str) or (required and not candidate):
        raise LiveConfigError(f"{name} must be a non-empty string")
    if len(candidate) > maximum or any(
            ord(character) < 0x20 for character in candidate):
        raise LiveConfigError(f"{name} contains invalid characters")
    return candidate


@dataclass(frozen=True)
class LiveReceiverConfig:
    host: str
    port: int = 1883
    username: str = ""
    password: str = field(default="", repr=False)
    client_id: str = ""
    uplink_topic: str = "dji-h1/test/up"
    ack_topic: str = "dji-h1/test/down"
    subscribe_qos: int = 1
    expected_uplink_qos: int = 1
    ack_qos: int = 0
    keepalive_seconds: int = 30
    max_inflight: int = 512
    ingress_queue_size: int = 512
    ack_queue_size: int = 512
    source_id: int | None = None
    mission_id: int | None = None
    timezone_name: str = "Asia/Shanghai"
    timezone_offset_minutes: int = 480
    tls: bool = False
    tls_ca_file: str | None = None
    ack_enabled: bool = True
    reassembly_timeout_seconds: int = 30

    @classmethod
    def load(cls, path: str | Path) -> "LiveReceiverConfig":
        source = Path(path).expanduser().resolve()
        try:
            root = json.loads(source.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise LiveConfigError(f"cannot read live MQTT configuration: {exc}") from exc
        return cls.from_dict(root)

    @classmethod
    def from_dict(cls, root: dict) -> "LiveReceiverConfig":
        """Validate an already loaded configuration without any GUI imports."""
        if not isinstance(root, dict) or not isinstance(root.get("mqtt"), dict):
            raise LiveConfigError("configuration must contain an mqtt object")
        mqtt = root["mqtt"]
        for name in ("tls", "ack_enabled"):
            if name in mqtt and not isinstance(mqtt[name], bool):
                raise LiveConfigError(f"mqtt.{name} must be a boolean")
        viewer = root.get("viewer", {})
        if not isinstance(viewer, dict):
            raise LiveConfigError("viewer must be an object")

        client_id = _text(
            mqtt.get("client_id", f"DJI_H1_live_viewer_{os.getpid()}"),
            "mqtt.client_id", maximum=64)
        username = _text(mqtt.get("username", ""), "mqtt.username",
                         required=False)
        password = _text(mqtt.get("password", ""), "mqtt.password",
                         required=False, maximum=1024, strip=False)
        source_filter = mqtt.get("source_id")
        mission_filter = mqtt.get("mission_id")
        uplink_topic = _text(
            mqtt.get("uplink_topic", "dji-h1/test/up"), "mqtt.uplink_topic")
        ack_topic = _text(
            mqtt.get("ack_topic", "dji-h1/test/down"), "mqtt.ack_topic")
        if any(token in uplink_topic for token in ("+", "#")):
            raise LiveConfigError(
                "mqtt.uplink_topic must be an exact topic, not a wildcard")
        if any(token in ack_topic for token in ("+", "#")):
            raise LiveConfigError(
                "mqtt.ack_topic must be an exact publish topic")
        return cls(
            tls=mqtt.get("tls", False),
            tls_ca_file=(_text(mqtt["tls_ca_file"], "mqtt.tls_ca_file", maximum=4096)
                         if mqtt.get("tls_ca_file") else None),
            ack_enabled=mqtt.get("ack_enabled", True),
            reassembly_timeout_seconds=_integer(
                mqtt.get("reassembly_timeout_seconds", 30),
                "mqtt.reassembly_timeout_seconds", 1, 600),
            host=_text(mqtt.get("host"), "mqtt.host"),
            port=_integer(mqtt.get("port", 1883), "mqtt.port", 1, 65535),
            username=username, password=password, client_id=client_id,
            uplink_topic=uplink_topic,
            ack_topic=ack_topic,
            subscribe_qos=_integer(mqtt.get("subscribe_qos", 1),
                                   "mqtt.subscribe_qos", 0, 1),
            expected_uplink_qos=_integer(mqtt.get("expected_uplink_qos", 1),
                                         "mqtt.expected_uplink_qos", 0, 1),
            ack_qos=_integer(mqtt.get("ack_qos", 0),
                             "mqtt.ack_qos", 0, 1),
            keepalive_seconds=_integer(mqtt.get("keepalive_seconds", 30),
                                       "mqtt.keepalive_seconds", 5, 3600),
            max_inflight=_integer(mqtt.get("max_inflight", 512),
                                  "mqtt.max_inflight", 1, 4096),
            ingress_queue_size=_integer(
                mqtt.get("ingress_queue_size", 512),
                "mqtt.ingress_queue_size", 1, 8192),
            ack_queue_size=_integer(
                mqtt.get("ack_queue_size", 512),
                "mqtt.ack_queue_size", 1, 8192),
            source_id=(None if source_filter is None else
                       _integer(source_filter, "mqtt.source_id", 1,
                                0xFFFFFFFFFFFFFFFF)),
            mission_id=(None if mission_filter is None else
                        _integer(mission_filter, "mqtt.mission_id", 1,
                                 0xFFFFFFFFFFFFFFFF)),
            timezone_name=_text(viewer.get("timezone_name", "Asia/Shanghai"),
                                "viewer.timezone_name", maximum=63),
            timezone_offset_minutes=_integer(
                viewer.get("timezone_offset_minutes", 480),
                "viewer.timezone_offset_minutes", -720, 840),
        )


class _MemoryRecordFile:
    """RecordFile-compatible immutable collection for a live snapshot."""

    def __init__(self, record_type: int,
                 entries: tuple[GpsRecord | RawSpectrum |
                                ReflectanceSpectrum, ...]):
        self.header = FileHeader(record_type)
        self.crc_verified = True
        self.scan_issue = None
        self._entries = entries
        self.records = tuple(
            RecordRef(
                entry.header, 0, 0,
                entry.header.record_size - 64,
                entry.sample if isinstance(entry, GpsRecord) else entry.info,
                0,
            ) for entry in entries
        )
        self.byte_size = 16 + sum(entry.header.record_size for entry in entries)

    def __len__(self) -> int:
        return len(self._entries)

    def raw_spectrum(self, index: int) -> RawSpectrum:
        entry = self._entries[index]
        if not isinstance(entry, RawSpectrum):
            raise TypeError("record collection does not contain raw spectra")
        return entry

    def reflectance_spectrum(self, index: int) -> ReflectanceSpectrum:
        entry = self._entries[index]
        if not isinstance(entry, ReflectanceSpectrum):
            raise TypeError("record collection does not contain reflectance spectra")
        return entry


@dataclass
class _MutableMission:
    source_id: int
    mission_id: int
    gps: OrderedDict[int, GpsRecord] = field(default_factory=OrderedDict)
    reflectance: OrderedDict[int, ReflectanceSpectrum] = field(
        default_factory=OrderedDict)
    events: OrderedDict[int, OperationEvent] = field(
        default_factory=OrderedDict)
    latest_utc_ms: int = 0
    raw: OrderedDict[tuple[int, int], RawSpectrum] = field(default_factory=OrderedDict)
    last_arrival: int = 0


class LiveMissionStore:
    """Thread-safe live records with an offline-compatible Mission snapshot."""

    # ``accept`` historically returns a source-record count, where zero also
    # means an idempotent duplicate.  Keep that public contract while using a
    # distinct negative result for an identity filter: filtered traffic must
    # never receive the positive DTA1 that deliberately clears a B-board pool
    # entry.
    FILTERED = -1

    def __init__(self, config: LiveReceiverConfig):
        self._config = config
        self._lock = RLock()
        self._missions: OrderedDict[tuple[int, int], _MutableMission] = \
            OrderedDict()
        self._active_key: tuple[int, int] | None = None
        self._arrival = 0
        self._revision = 0
        self.filtered_records = 0

    @property
    def revision(self) -> int:
        """Return the immutable-snapshot generation currently available."""

        with self._lock:
            return self._revision

    def accept(self, message: ReassembledTelemetry) -> int:
        """Decode one message and return added count, or ``FILTERED``."""

        if message.message_type == MESSAGE_GPS_BATCH:
            wires = decode_gps_batch(message.payload)
            entries = tuple(decode_record(wire, expected_type=RECORD_GPS)
                            for wire in wires)
            if not entries or entries[0].header.sequence != message.message_sequence:
                raise RecordFormatError("DTF2/DGB1 batch identity mismatch")
        elif message.message_type == MESSAGE_GPS:
            entries = (decode_record(
                message.payload, expected_type=RECORD_GPS,
                expected_sequence=message.message_sequence),)
        elif message.message_type == MESSAGE_REFLECTANCE:
            entries = (decode_record(
                message.payload, expected_type=RECORD_REFLECTANCE,
                expected_sequence=message.message_sequence),)
        elif message.message_type == MESSAGE_OPERATION_LOG:
            entries = (decode_record(
                message.payload, expected_type=RECORD_OPERATION_LOG,
                expected_sequence=message.message_sequence),)
        elif message.message_type == RECORD_RAW_SPECTRUM:
            entries = (decode_record(
                message.payload, expected_type=RECORD_RAW_SPECTRUM,
                expected_sequence=message.message_sequence),)
        else:
            raise RecordFormatError(
                f"unsupported live telemetry type {message.message_type}")

        if self._config.source_id is not None and \
                message.source_id != self._config.source_id:
            with self._lock:
                self.filtered_records += len(entries)
            return self.FILTERED
        if self._config.mission_id is not None and \
                message.mission_id != self._config.mission_id:
            with self._lock:
                self.filtered_records += len(entries)
            return self.FILTERED

        key = (message.source_id, message.mission_id)
        with self._lock:
            mission = self._missions.get(key)
            if mission is None:
                mission = _MutableMission(*key)
                self._missions[key] = mission
                while len(self._missions) > 4:
                    oldest = next(iter(self._missions))
                    if oldest == self._active_key:
                        self._missions.move_to_end(oldest)
                        continue
                    del self._missions[oldest]
            self._arrival += 1
            mission.last_arrival = self._arrival
            added = 0
            for entry in entries:
                mission.latest_utc_ms = max(
                    mission.latest_utc_ms, entry.header.utc_ms)
                if isinstance(entry, GpsRecord):
                    if entry.header.sequence not in mission.gps:
                        mission.gps[entry.header.sequence] = entry
                        added += 1
                elif isinstance(entry, ReflectanceSpectrum):
                    if entry.header.sequence not in mission.reflectance:
                        mission.reflectance[entry.header.sequence] = entry
                        added += 1
                elif isinstance(entry, OperationEvent):
                    if entry.header.sequence not in mission.events:
                        mission.events[entry.header.sequence] = entry
                        added += 1
                elif isinstance(entry, RawSpectrum):
                    raw_key = (entry.info.spectrometer_role, entry.header.sequence)
                    if raw_key not in mission.raw:
                        mission.raw[raw_key] = entry
                        added += 1
            self._select_active(key)
            if added:
                self._revision += 1
            return added

    def _select_active(self, candidate_key: tuple[int, int]) -> None:
        if self._active_key is None:
            self._active_key = candidate_key
            return
        active = self._missions[self._active_key]
        candidate = self._missions[candidate_key]
        active_rank = (bool(active.latest_utc_ms), active.latest_utc_ms,
                       active.last_arrival if not active.latest_utc_ms else 0)
        candidate_rank = (bool(candidate.latest_utc_ms), candidate.latest_utc_ms,
                          candidate.last_arrival if not candidate.latest_utc_ms else 0)
        if candidate_rank > active_rank:
            self._active_key = candidate_key

    def snapshot(self, receiver_status: dict[str, Any]) -> Mission | None:
        with self._lock:
            if self._active_key is None:
                return None
            source_id, mission_id = self._active_key
            mutable = self._missions[self._active_key]
            gps_entries = tuple(mutable.gps.values())
            raw_entries = tuple(mutable.raw.values())
            reflectance_entries = tuple(mutable.reflectance.values())
            event_entries = tuple(sorted(
                mutable.events.values(),
                key=lambda entry: (entry.header.b_monotonic_us,
                                   entry.header.sequence)))
            timestamps = [entry.header.utc_ms for entry in
                          (*gps_entries, *reflectance_entries, *event_entries)
                          if entry.header.utc_ms > 0]
            summary = {
                "schema_version": 1,
                "directory": f"LIVE_{mission_id:016X}",
                "state": ("receiving" if receiver_status.get("connected")
                          else "disconnected"),
                "source_id": source_id,
                "source_id_hex": f"{source_id:016X}",
                "mission_id": mission_id,
                "mission_id_hex": f"{mission_id:016X}",
                "drone_serial": "live telemetry",
                "drone_serial_hex": f"source:{source_id:016X}",
                "firmware_version": "reported by SD summary only",
                "started_utc_ms": min(timestamps) if timestamps else 0,
                "updated_utc_ms": max(timestamps) if timestamps else 0,
                "timezone": {
                    "name": self._config.timezone_name,
                    "utc_offset_minutes": self._config.timezone_offset_minutes,
                },
                "live_telemetry": {
                    **receiver_status,
                    "store_revision": self._revision,
                    "active_source_id": source_id,
                    "active_mission_id": mission_id,
                    "filtered_records": self.filtered_records,
                },
            }
        events = [_event_dictionary(entry) for entry in event_entries]
        return Mission(
            Path(summary["directory"]), summary, "live MQTT", events, [], [],
            (_MemoryRecordFile(RECORD_RAW_SPECTRUM, raw_entries) if raw_entries else None),
            (_MemoryRecordFile(RECORD_REFLECTANCE, reflectance_entries)
             if reflectance_entries else None),
            (_MemoryRecordFile(RECORD_GPS, gps_entries)
             if gps_entries else None),
        )


@dataclass(frozen=True)
class _InboundPublication:
    topic: str
    payload: bytes
    qos: int
    received_monotonic: float
    received_utc_ns: int = 0


class LiveTelemetrySource:
    """Decoupled MQTT ingress, telemetry processor, and snapshot producer."""

    _ACK_CACHE_MAX = 4096
    _WORKER_JOIN_SECONDS = 5.0

    def __init__(self, config: LiveReceiverConfig):
        self.config = config
        self.store = LiveMissionStore(config)
        self._lock = RLock()
        self._stream = TelemetryFragmentStreamDecoder()
        self._reassembler = TelemetryReassembler(
            timeout_seconds=config.reassembly_timeout_seconds, max_inflight=config.max_inflight)
        self._ack_cache: OrderedDict[tuple[int, ...], bytes] = OrderedDict()
        self._ingress: Queue[_InboundPublication] = Queue(
            maxsize=config.ingress_queue_size)
        self._ack_egress: Queue[bytes] = Queue(
            maxsize=config.ack_queue_size)
        self._processor_stop = Event()
        self._ack_stop = Event()
        self._processor_thread: Thread | None = None
        self._ack_thread: Thread | None = None
        self._running = False
        self._accepting = False
        self._last_message_monotonic: float | None = None
        self._status: dict[str, Any] = {
            "connected": False, "state": "stopped", "last_error": "",
            "mqtt_messages": 0, "mqtt_bytes": 0, "fragments": 0,
            "complete_messages": 0, "gps_records": 0,
            "reflectance_records": 0, "event_records": 0, "raw_records": 0,
            "duplicate_fragments": 0,
            "invalid_messages": 0, "expired_assemblies": 0,
            "acknowledgements": 0, "ack_publish_failures": 0,
            "qos_mismatches": 0, "ingress_dropped": 0,
            "ingress_high_water": 0, "processed_publications": 0,
            "processing_failures": 0, "processing_last_ms": 0.0,
            "processing_max_ms": 0.0, "ack_queue_overflows": 0,
            "ack_queue_high_water": 0,
            "inflight": 0, "buffered_bytes": 0,
            "last_message_age_seconds": None,
        }
        self._transport = LiveMqttTransport(
            config, self._enqueue_publication, self._transport_state)

    @property
    def description(self) -> str:
        return (f"mqtt://{self.config.host}:{self.config.port}/"
                f"{self.config.uplink_topic}")

    @property
    def revision(self) -> int:
        return self.store.revision

    def start(self) -> None:
        with self._lock:
            if self._running:
                return
            self._running = True
            self._accepting = True
            self._status["state"] = "starting"
            self._status["last_error"] = ""
        self._start_workers()
        try:
            self._transport.start()
        except Exception:
            with self._lock:
                self._running = False
                self._accepting = False
                self._status["state"] = "failed"
            self._stop_workers()
            raise

    def stop(self) -> None:
        # Stop accepting first, drain work while MQTT remains available for
        # final DTA1 publications, then close the transport.
        with self._lock:
            self._accepting = False
            self._running = False
        self._stop_workers()
        self._transport.stop()
        with self._lock:
            self._status["connected"] = False
            self._status["state"] = "stopped"

    def status(self) -> dict[str, Any]:
        with self._lock:
            result = dict(self._status)
            result["last_message_age_seconds"] = (
                None if self._last_message_monotonic is None else
                max(0.0, time.monotonic() - self._last_message_monotonic))
        result["ingress_queue_depth"] = self._ingress.qsize()
        result["ack_queue_depth"] = self._ack_egress.qsize()
        result["store_revision"] = self.store.revision
        return result

    def snapshot(self, receiver_status: dict[str, Any] | None = None) \
            -> Mission | None:
        return self.store.snapshot(
            self.status() if receiver_status is None else receiver_status)

    def _start_workers(self) -> None:
        self._processor_stop.clear()
        self._ack_stop.clear()
        self._processor_thread = Thread(
            target=self._processor_loop, name="DJI-H1 telemetry decoder",
            daemon=True)
        self._ack_thread = Thread(
            target=self._ack_loop, name="DJI-H1 acknowledgement publisher",
            daemon=True)
        self._processor_thread.start()
        self._ack_thread.start()

    def _stop_workers(self) -> None:
        self._processor_stop.set()
        processor = self._processor_thread
        if processor is not None:
            processor.join(self._WORKER_JOIN_SECONDS)
            if processor.is_alive():
                self._set_error("telemetry processor did not stop cleanly")
        self._ack_stop.set()
        acknowledger = self._ack_thread
        if acknowledger is not None:
            acknowledger.join(self._WORKER_JOIN_SECONDS)
            if acknowledger.is_alive():
                self._set_error("acknowledgement publisher did not stop cleanly")
        self._processor_thread = None
        self._ack_thread = None

    def _transport_state(self, state: str, connected: bool,
                         error: str) -> None:
        with self._lock:
            self._status["state"] = state
            self._status["connected"] = connected
            if error:
                self._status["last_error"] = error

    def _set_error(self, message: str) -> None:
        with self._lock:
            self._status["last_error"] = message

    def _enqueue_publication(self, topic: str, payload: bytes, qos: int) -> None:
        """Paho callback target: account, enqueue, and return immediately."""

        if topic != self.config.uplink_topic:
            return
        now = time.monotonic()
        with self._lock:
            if not self._accepting:
                return
            self._last_message_monotonic = now
            self._status["mqtt_messages"] += 1
            self._status["mqtt_bytes"] += len(payload)
            if qos != self.config.expected_uplink_qos:
                self._status["qos_mismatches"] += 1
            publication = _InboundPublication(topic, bytes(payload), qos, now, time.time_ns())
            try:
                self._ingress.put_nowait(publication)
            except Full:
                # Returning allows MQTT QoS-1 delivery to complete, but deliberately
                # omitting DTA1 makes the B-board retain/retry the logical message.
                with self._lock:
                    self._status["ingress_dropped"] += 1
                    self._status["last_error"] = "telemetry ingress queue full"
                return
            depth = self._ingress.qsize()
            with self._lock:
                self._status["ingress_high_water"] = max(
                    self._status["ingress_high_water"], depth)

    def _processor_loop(self) -> None:
        while not self._processor_stop.is_set() or not self._ingress.empty():
            try:
                publication = self._ingress.get(timeout=0.1)
            except Empty:
                self._expire_assemblies()
                continue
            started = time.perf_counter()
            try:
                self._process_publication(publication)
            except Exception as exc:  # Keep the receiver alive on bad input.
                with self._lock:
                    self._status["processing_failures"] += 1
                    self._status["last_error"] = (
                        f"telemetry processor failure: {exc}")
            finally:
                elapsed_ms = (time.perf_counter() - started) * 1000.0
                with self._lock:
                    self._status["processed_publications"] += 1
                    self._status["processing_last_ms"] = elapsed_ms
                    self._status["processing_max_ms"] = max(
                        self._status["processing_max_ms"], elapsed_ms)
                    self._status["inflight"] = \
                        self._reassembler.inflight_count
                    self._status["buffered_bytes"] = \
                        self._stream.buffered_bytes
                self._ingress.task_done()

    def _expire_assemblies(self) -> None:
        expired = self._reassembler.expire()
        with self._lock:
            if expired:
                self._status["expired_assemblies"] += expired
            self._status["inflight"] = self._reassembler.inflight_count
            self._status["buffered_bytes"] = self._stream.buffered_bytes

    def _process_publication(self, publication: _InboundPublication) -> None:
        try:
            if publication.payload.startswith(MESSAGE_MAGIC_BYTES):
                self._process_complete_message(
                    decode_message_envelope(publication.payload), publication)
                return
            fragments = self._stream.feed(publication.payload)
            for fragment in fragments:
                with self._lock:
                    self._status["fragments"] += 1
                key = self._fragment_key(fragment)
                cached = self._ack_cache.get(key)
                if cached is not None:
                    with self._lock:
                        self._status["duplicate_fragments"] += 1
                    self._ack_cache.move_to_end(key)
                    if fragment.fragment_index == 0:
                        self._queue_ack(cached)
                    continue
                completed = self._reassembler.push(fragment, now=publication.received_monotonic)
                if completed is None:
                    continue
                try:
                    added = self._accept_message(completed, publication)
                    if added == LiveMissionStore.FILTERED:
                        # This receiver is not an acceptance authority for the
                        # excluded identity.  Do not cache or publish DTA1: an
                        # intended receiver on the topic must acknowledge it.
                        continue
                    encoded_ack = encode_acknowledgement(completed)
                except (FragmentError, RecordFormatError, ValueError) as exc:
                    with self._lock:
                        self._status["invalid_messages"] += 1
                        self._status["last_error"] = str(exc)
                    encoded_ack = encode_acknowledgement(
                        completed, status=ACK_STATUS_PERMANENT_REJECTION)
                except Exception:
                    # Retry must remain possible after transient storage failure.
                    self._reassembler.forget_completed(completed)
                    raise
                else:
                    with self._lock:
                        self._status["complete_messages"] += 1
                        if completed.message_type in (
                                MESSAGE_GPS, MESSAGE_GPS_BATCH):
                            self._status["gps_records"] += added
                        elif completed.message_type == MESSAGE_REFLECTANCE:
                            self._status["reflectance_records"] += added
                        elif completed.message_type == MESSAGE_OPERATION_LOG:
                            self._status["event_records"] += added
                        elif completed.message_type == RECORD_RAW_SPECTRUM:
                            self._status["raw_records"] += added
                self._ack_cache[key] = encoded_ack
                self._ack_cache.move_to_end(key)
                if len(self._ack_cache) > self._ACK_CACHE_MAX:
                    self._ack_cache.popitem(last=False)
                self._queue_ack(encoded_ack)
        except (FragmentError, ValueError) as exc:
            with self._lock:
                self._status["invalid_messages"] += 1
                self._status["last_error"] = str(exc)

    def _process_complete_message(self, completed, publication):
        # DTM1 preserves MQTT boundaries. No stream buffer or reassembly state.
        # Validate the entire publication even for a cached duplicate.
        import zlib
        key = (completed.source_id, completed.mission_id, completed.message_type,
               completed.message_sequence, len(completed.payload),
               zlib.crc32(completed.payload) & 0xFFFFFFFF, 1, completed.flags)
        cached = self._ack_cache.get(key)
        if cached is not None:
            with self._lock:
                self._status["duplicate_fragments"] += 1
            self._ack_cache.move_to_end(key)
            self._queue_ack(cached)
            return
        try:
            added = self._accept_message(completed, publication)
            if added == LiveMissionStore.FILTERED:
                return
            encoded = encode_acknowledgement(completed)
        except (FragmentError, RecordFormatError, ValueError) as exc:
            with self._lock:
                self._status["invalid_messages"] += 1
                self._status["last_error"] = str(exc)
            encoded = encode_acknowledgement(completed, status=ACK_STATUS_PERMANENT_REJECTION)
        else:
            with self._lock:
                self._status["complete_messages"] += 1
                counter = {MESSAGE_GPS: "gps_records", MESSAGE_GPS_BATCH: "gps_records",
                           MESSAGE_REFLECTANCE: "reflectance_records",
                           MESSAGE_OPERATION_LOG: "event_records",
                           RECORD_RAW_SPECTRUM: "raw_records"}[completed.message_type]
                self._status[counter] += added
        # Transient storage errors escape: no cache entry, no success ACK.
        self._ack_cache[key] = encoded
        if len(self._ack_cache) > self._ACK_CACHE_MAX:
            self._ack_cache.popitem(last=False)
        self._queue_ack(encoded)

    def _queue_ack(self, encoded: bytes) -> None:
        if not self.config.ack_enabled:
            return
        try:
            self._ack_egress.put_nowait(encoded)
        except Full:
            with self._lock:
                self._status["ack_queue_overflows"] += 1
                self._status["last_error"] = "acknowledgement queue full"
            return
        depth = self._ack_egress.qsize()
        with self._lock:
            self._status["ack_queue_high_water"] = max(
                self._status["ack_queue_high_water"], depth)

    def _ack_loop(self) -> None:
        while not self._ack_stop.is_set() or not self._ack_egress.empty():
            try:
                encoded = self._ack_egress.get(timeout=0.1)
            except Empty:
                continue
            try:
                self._transport.publish(
                    self.config.ack_topic, encoded, self.config.ack_qos)
            except (OSError, RuntimeError, ValueError) as exc:
                with self._lock:
                    self._status["ack_publish_failures"] += 1
                    self._status["last_error"] = str(exc)
            else:
                with self._lock:
                    self._status["acknowledgements"] += 1
            finally:
                self._ack_egress.task_done()

    @staticmethod
    def _fragment_key(fragment) -> tuple[int, ...]:
        return (
            fragment.source_id, fragment.mission_id, fragment.message_type,
            fragment.message_sequence, fragment.message_length,
            fragment.message_crc32, fragment.fragment_count, fragment.flags,
        )

    def _accept_message(self, completed, publication) -> int:
        """Extension point executed only by the decoder worker, before DTA1."""
        return self.store.accept(completed)
