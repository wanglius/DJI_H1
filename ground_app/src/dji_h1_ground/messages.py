"""Typed, Qt-free output for a complete DTF2 logical message."""
from __future__ import annotations

import base64
from dataclasses import asdict, dataclass
from typing import Any

from .decoder import decode_record, RecordFormatError
from .telemetry import ReassembledTelemetry, MESSAGE_GPS_BATCH, decode_gps_batch


def _json_value(value):
    if isinstance(value, dict):
        return {key: _json_value(item) for key, item in value.items()}
    if isinstance(value, (tuple, list, bytes)):
        return [_json_value(item) for item in value]
    return value


@dataclass(frozen=True)
class DecodedMessage:
    source_id: int
    mission_id: int
    message_type: int
    message_sequence: int
    fragment_count: int
    received_utc_ns: int
    received_monotonic: float
    topic: str
    qos: int
    records: tuple[Any, ...]
    payload: bytes

    def to_dict(self) -> dict:
        """JSON-safe, explicit units; hex identity fields are safe in JavaScript."""
        return {
            "schema_version": 1,
            "source_id_hex": f"{self.source_id:016X}",
            "mission_id_hex": f"{self.mission_id:016X}",
            "message_type": self.message_type,
            "message_sequence": self.message_sequence,
            "fragment_count": self.fragment_count,
            "received_utc_ns": str(self.received_utc_ns),
            "received_monotonic": self.received_monotonic,
            "topic": self.topic, "qos": self.qos,
            "records": [_json_value(asdict(record)) for record in self.records],
            "payload_base64": base64.b64encode(self.payload).decode("ascii"),
        }


def decode_message(message: ReassembledTelemetry, *, received_utc_ns=0,
                   received_monotonic=0.0, topic="", qos=0) -> DecodedMessage:
    """Validate DHR1/CRC or expand DGB1; unknown types raise RecordFormatError.

    The receiver still journals unknown/invalid wire data for future decoding.
    No MQTT, map, filesystem or application callback is involved here.
    """
    if message.message_type == MESSAGE_GPS_BATCH:
        wires = decode_gps_batch(message.payload)
        records = tuple(decode_record(wire, expected_type=1) for wire in wires)
        if not records or records[0].header.sequence != message.message_sequence:
            raise RecordFormatError("DTF2/DGB1 batch identity mismatch")
    else:
        records = (decode_record(message.payload, expected_type=message.message_type,
                                 expected_sequence=message.message_sequence),)
    return DecodedMessage(message.source_id, message.mission_id,
                          message.message_type, message.message_sequence,
                          message.fragment_count, received_utc_ns,
                          received_monotonic, topic, qos, records, message.payload)
