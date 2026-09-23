"""Single JSON configuration. Loading a bad map setting cannot stop telemetry."""
from __future__ import annotations

from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import uuid

from .live import LiveConfigError, LiveReceiverConfig, _integer


@dataclass(frozen=True)
class MapConfig:
    enabled: bool = False
    ak: str = field(default="", repr=False)
    allow_mission_coordinates: bool = False
    load_timeout_ms: int = 15000
    error: str = ""

    @classmethod
    def parse(cls, value):
        try:
            if not isinstance(value, dict):
                raise ValueError("map must be an object")
            enabled = value.get("enabled", False)
            allow = value.get("allow_mission_coordinates", False)
            if not isinstance(enabled, bool) or not isinstance(allow, bool):
                raise ValueError("map switches must be booleans")
            ak = os.environ.get("DJI_H1_BAIDU_AK", value.get("ak", ""))
            if enabled and (not isinstance(ak, str) or
                            not re.fullmatch(r"[A-Za-z0-9_-]{10,128}", ak)):
                raise ValueError("missing or invalid Baidu AK")
            timeout = _integer(value.get("load_timeout_ms", 15000),
                               "map.load_timeout_ms", 1000, 120000)
            return cls(enabled, ak if isinstance(ak, str) else "", allow, timeout)
        except (TypeError, ValueError):
            # Never include the offending value: it may contain a credential.
            return cls(error="Invalid map settings; using offline map")


@dataclass(frozen=True)
class GroundConfig:
    mqtt: LiveReceiverConfig
    map: MapConfig = field(default_factory=MapConfig)
    refresh_ms: int = 1000
    api_enabled: bool = True
    api_port: int = 8765
    storage_enabled: bool = True
    storage_directory: Path = Path("data")
    output_queue_size: int = 512

    @classmethod
    def load(cls, path: str | Path):
        path = Path(path).expanduser().resolve()
        try:
            root = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            raise LiveConfigError("Cannot read config JSON; check path and syntax") from None
        if not isinstance(root, dict) or root.get("schema_version") != 1:
            raise LiveConfigError("schema_version must be 1")
        sections = {}
        for name in ("mqtt", "viewer", "api", "storage"):
            sections[name] = root.get(name, {})
            if not isinstance(sections[name], dict):
                raise LiveConfigError(f"{name} must be an object")
        mqtt = dict(sections["mqtt"])
        if not mqtt.get("client_id"):
            mqtt["client_id"] = "DJI_H1_ground_" + uuid.uuid4().hex[:16]
        if "DJI_H1_MQTT_PASSWORD" in os.environ:
            mqtt["password"] = os.environ["DJI_H1_MQTT_PASSWORD"]
        ca = mqtt.get("tls_ca_file")
        if ca:
            if not isinstance(ca, str):
                raise LiveConfigError("mqtt.tls_ca_file must be a path")
            mqtt["tls_ca_file"] = str((path.parent / ca).resolve())
        receiver = LiveReceiverConfig.from_dict({**root, "mqtt": mqtt})
        api, storage, viewer = (sections[n] for n in ("api", "storage", "viewer"))
        for name, section in (("api", api), ("storage", storage)):
            if not isinstance(section.get("enabled", True), bool):
                raise LiveConfigError(f"{name}.enabled must be boolean")
        directory = storage.get("directory", "../data")
        if not isinstance(directory, str) or not directory:
            raise LiveConfigError("storage.directory must be a path")
        return cls(
            receiver, MapConfig.parse(root.get("map", {})),
            _integer(viewer.get("refresh_ms", 1000), "viewer.refresh_ms", 100, 60000),
            api.get("enabled", True),
            _integer(api.get("port", 8765), "api.port", 0, 65535),
            storage.get("enabled", True), (path.parent / directory).resolve(),
            _integer(storage.get("output_queue_size", 512),
                     "storage.output_queue_size", 1, 8192))
