"""Version-02 MQTT telemetry fragmentation and reassembly APIs."""

from __future__ import annotations

from dataclasses import dataclass, field
import struct
import time
from typing import Callable
import zlib


FRAGMENT_MAGIC = 0x32465444  # Little-endian bytes: DTF2.
FRAGMENT_VERSION = 2
FRAGMENT_HEADER_SIZE = 44
FRAGMENT_TRAILER_SIZE = 4
FRAGMENT_WIRE_MAX_SIZE = 1024
FRAGMENT_PAYLOAD_MAX = (
    FRAGMENT_WIRE_MAX_SIZE - FRAGMENT_HEADER_SIZE - FRAGMENT_TRAILER_SIZE
)
MESSAGE_MAX_SIZE = FRAGMENT_PAYLOAD_MAX * 0xFFFF
DEFAULT_REASSEMBLY_MESSAGE_MAX = 64 * 1024

MESSAGE_GPS = 1
MESSAGE_RAW_SPECTRUM = 2
MESSAGE_REFLECTANCE = 3
MESSAGE_OPERATION_LOG = 4

_HEADER = struct.Struct("<IBBHQQIIIHHHH")
_CRC = struct.Struct("<I")
_MAGIC_BYTES = struct.pack("<I", FRAGMENT_MAGIC)
ACK_MAGIC = 0x31415444  # Little-endian bytes: DTA1.
ACK_VERSION = 1
ACK_WIRE_SIZE = 40
_ACK = struct.Struct("<IBBHQQIIHHI")


class FragmentError(ValueError):
    """Raised when fragment framing or reassembly integrity is invalid."""


def _copy_bytes(value: object, description: str) -> bytes:
    """Copy a buffer object without treating an integer as a byte count."""
    try:
        return memoryview(value).tobytes()
    except (TypeError, ValueError) as exc:
        raise FragmentError(f"{description} must be bytes-like") from exc


@dataclass(frozen=True)
class TelemetryFragment:
    message_type: int
    source_id: int
    mission_id: int
    message_sequence: int
    message_length: int
    message_crc32: int
    fragment_index: int
    fragment_count: int
    flags: int
    payload: bytes

    @property
    def payload_length(self) -> int:
        return len(self.payload)


@dataclass(frozen=True)
class ReassembledTelemetry:
    message_type: int
    source_id: int
    mission_id: int
    message_sequence: int
    flags: int
    fragment_count: int
    payload: bytes


def _uint(name: str, value: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise FragmentError(f"{name} must be an integer")
    if not 0 <= value <= maximum:
        raise FragmentError(f"{name} must be in 0..{maximum}")
    return value


def _fragment_count(message_length: int) -> int:
    return max(1, (message_length + FRAGMENT_PAYLOAD_MAX - 1) //
               FRAGMENT_PAYLOAD_MAX)


def fragment_message(
    message_type: int,
    mission_id: int,
    message_sequence: int,
    payload: bytes,
    *,
    source_id: int = 1,
    flags: int = 0,
) -> tuple[bytes, ...]:
    """Encode one logical telemetry message into <=1024-byte fragments."""
    message_type = _uint("message_type", message_type, 0xFF)
    if message_type == 0:
        raise FragmentError("message_type zero is reserved")
    mission_id = _uint("mission_id", mission_id, 0xFFFFFFFFFFFFFFFF)
    if mission_id == 0:
        raise FragmentError("mission_id zero is reserved")
    source_id = _uint("source_id", source_id, 0xFFFFFFFFFFFFFFFF)
    if source_id == 0:
        raise FragmentError("source_id zero is reserved")
    message_sequence = _uint(
        "message_sequence", message_sequence, 0xFFFFFFFF
    )
    flags = _uint("flags", flags, 0xFFFF)
    payload = _copy_bytes(payload, "payload")
    if len(payload) > MESSAGE_MAX_SIZE:
        raise FragmentError(f"payload exceeds {MESSAGE_MAX_SIZE} bytes")

    message_crc = zlib.crc32(payload) & 0xFFFFFFFF
    count = _fragment_count(len(payload))
    encoded: list[bytes] = []
    for index in range(count):
        start = index * FRAGMENT_PAYLOAD_MAX
        part = payload[start:start + FRAGMENT_PAYLOAD_MAX]
        header = _HEADER.pack(
            FRAGMENT_MAGIC, FRAGMENT_VERSION, message_type,
            FRAGMENT_HEADER_SIZE, source_id, mission_id, message_sequence,
            len(payload), message_crc, index, count, len(part), flags,
        )
        without_crc = header + part
        encoded.append(
            without_crc + _CRC.pack(zlib.crc32(without_crc) & 0xFFFFFFFF)
        )
    return tuple(encoded)


def decode_fragment(encoded: bytes) -> TelemetryFragment:
    """Validate and decode one complete transport fragment."""
    encoded = _copy_bytes(encoded, "encoded fragment")
    if not FRAGMENT_HEADER_SIZE + FRAGMENT_TRAILER_SIZE <= len(encoded) <= \
            FRAGMENT_WIRE_MAX_SIZE:
        raise FragmentError("invalid fragment length")

    stored_crc, = _CRC.unpack_from(encoded, len(encoded) - _CRC.size)
    calculated_crc = zlib.crc32(encoded[:-_CRC.size]) & 0xFFFFFFFF
    if stored_crc != calculated_crc:
        raise FragmentError("fragment CRC mismatch")

    (magic, version, message_type, header_size, source_id, mission_id, sequence,
     message_length, message_crc, index, count, payload_length,
     flags) = _HEADER.unpack_from(encoded)
    if magic != FRAGMENT_MAGIC:
        raise FragmentError("invalid fragment magic")
    if version != FRAGMENT_VERSION:
        raise FragmentError(f"unsupported fragment version {version}")
    if message_type == 0:
        raise FragmentError("message_type zero is reserved")
    if source_id == 0:
        raise FragmentError("source_id zero is reserved")
    if mission_id == 0:
        raise FragmentError("mission_id zero is reserved")
    if header_size != FRAGMENT_HEADER_SIZE:
        raise FragmentError("invalid fragment header size")
    if message_length > MESSAGE_MAX_SIZE:
        raise FragmentError("declared message is too large")
    expected_count = _fragment_count(message_length)
    if count != expected_count or index >= count:
        raise FragmentError("invalid fragment index/count")
    expected_length = min(
        FRAGMENT_PAYLOAD_MAX,
        max(0, message_length - index * FRAGMENT_PAYLOAD_MAX),
    )
    if payload_length != expected_length:
        raise FragmentError("invalid fragment payload length")
    if len(encoded) != header_size + payload_length + _CRC.size:
        raise FragmentError("fragment wire length mismatch")
    return TelemetryFragment(
        message_type=message_type,
        source_id=source_id,
        mission_id=mission_id,
        message_sequence=sequence,
        message_length=message_length,
        message_crc32=message_crc,
        fragment_index=index,
        fragment_count=count,
        flags=flags,
        payload=encoded[header_size:header_size + payload_length],
    )


class TelemetryFragmentStreamDecoder:
    """Recover complete DTF2 fragments across arbitrary MQTT payload chunks."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    @property
    def buffered_bytes(self) -> int:
        return len(self._buffer)

    def reset(self) -> None:
        self._buffer.clear()

    def feed(self, data: bytes) -> list[TelemetryFragment]:
        try:
            self._buffer.extend(memoryview(data))
        except (TypeError, ValueError) as exc:
            raise FragmentError("stream input must be bytes-like") from exc

        decoded: list[TelemetryFragment] = []
        while True:
            start = self._buffer.find(_MAGIC_BYTES)
            if start < 0:
                # Retain the longest suffix that could begin a split magic.
                keep = min(len(self._buffer), len(_MAGIC_BYTES) - 1)
                if len(self._buffer) > keep:
                    del self._buffer[:-keep]
                break
            if start:
                del self._buffer[:start]
            if len(self._buffer) < FRAGMENT_HEADER_SIZE:
                break

            try:
                fields = _HEADER.unpack_from(self._buffer)
            except struct.error:
                break
            version = fields[1]
            header_size = fields[3]
            payload_length = fields[11]
            wire_length = header_size + payload_length + FRAGMENT_TRAILER_SIZE
            if (version != FRAGMENT_VERSION or
                    header_size != FRAGMENT_HEADER_SIZE or
                    payload_length > FRAGMENT_PAYLOAD_MAX or
                    wire_length > FRAGMENT_WIRE_MAX_SIZE):
                del self._buffer[0]
                continue
            if len(self._buffer) < wire_length:
                break

            candidate = bytes(self._buffer[:wire_length])
            try:
                fragment = decode_fragment(candidate)
            except FragmentError:
                # A damaged candidate must not hide a later valid magic.
                del self._buffer[0]
                continue
            decoded.append(fragment)
            del self._buffer[:wire_length]
        return decoded


@dataclass
class _Assembly:
    message_type: int
    source_id: int
    mission_id: int
    message_sequence: int
    message_length: int
    message_crc32: int
    fragment_count: int
    flags: int
    updated_at: float
    fragments: dict[int, bytes] = field(default_factory=dict)

    @classmethod
    def from_fragment(cls, fragment: TelemetryFragment, now: float) -> "_Assembly":
        return cls(
            message_type=fragment.message_type,
            source_id=fragment.source_id,
            mission_id=fragment.mission_id,
            message_sequence=fragment.message_sequence,
            message_length=fragment.message_length,
            message_crc32=fragment.message_crc32,
            fragment_count=fragment.fragment_count,
            flags=fragment.flags,
            updated_at=now,
        )

    def metadata(self) -> tuple[int, ...]:
        return (
            self.message_length, self.message_crc32,
            self.fragment_count, self.flags,
        )


class TelemetryReassembler:
    """Reassemble out-of-order fragments and suppress QoS 1 duplicates."""

    def __init__(
        self, *, timeout_seconds: float = 30.0, max_inflight: int = 16,
        max_message_size: int = DEFAULT_REASSEMBLY_MESSAGE_MAX,
        dedup_seconds: float = 300.0, max_completed: int = 1024,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be positive")
        if max_inflight <= 0:
            raise ValueError("max_inflight must be positive")
        if not 0 <= max_message_size <= MESSAGE_MAX_SIZE:
            raise ValueError(
                f"max_message_size must be in 0..{MESSAGE_MAX_SIZE}"
            )
        if dedup_seconds <= 0:
            raise ValueError("dedup_seconds must be positive")
        if max_completed <= 0:
            raise ValueError("max_completed must be positive")
        self.timeout_seconds = float(timeout_seconds)
        self.max_inflight = max_inflight
        self.max_message_size = max_message_size
        self.dedup_seconds = float(dedup_seconds)
        self.max_completed = max_completed
        self._clock = clock
        self._assemblies: dict[tuple[int, int, int, int], _Assembly] = {}
        self._completed: dict[
            tuple[int, int, int, int], tuple[tuple[int, ...], float]
        ] = {}

    @property
    def inflight_count(self) -> int:
        return len(self._assemblies)

    def expire(self, *, now: float | None = None) -> int:
        """Discard stale incomplete messages and return the number removed."""
        instant = self._clock() if now is None else now
        stale = [
            key for key, assembly in self._assemblies.items()
            if instant - assembly.updated_at >= self.timeout_seconds
        ]
        for key in stale:
            del self._assemblies[key]
        completed_stale = [
            key for key, (_, completed_at) in self._completed.items()
            if instant - completed_at >= self.dedup_seconds
        ]
        for key in completed_stale:
            del self._completed[key]
        return len(stale)

    def reset(self) -> None:
        self._assemblies.clear()
        self._completed.clear()

    def push(
        self, encoded: bytes | TelemetryFragment, *, now: float | None = None,
    ) -> ReassembledTelemetry | None:
        """Accept one fragment; return a message only when it becomes complete."""
        fragment = (encoded if isinstance(encoded, TelemetryFragment)
                    else decode_fragment(encoded))
        instant = self._clock() if now is None else now
        self.expire(now=instant)
        key = (
            fragment.source_id, fragment.mission_id, fragment.message_type,
            fragment.message_sequence,
        )
        fragment_metadata = (
            fragment.message_length, fragment.message_crc32,
            fragment.fragment_count, fragment.flags,
        )
        completed = self._completed.get(key)
        if completed is not None:
            completed_metadata, _ = completed
            if completed_metadata != fragment_metadata:
                raise FragmentError("completed message identity was reused")
            return None
        if fragment.message_length > self.max_message_size:
            raise FragmentError("declared message exceeds receiver limit")
        assembly = self._assemblies.get(key)
        if assembly is None:
            if len(self._assemblies) >= self.max_inflight:
                raise FragmentError("too many incomplete telemetry messages")
            assembly = _Assembly.from_fragment(fragment, instant)
            self._assemblies[key] = assembly
        if assembly.metadata() != fragment_metadata:
            del self._assemblies[key]
            raise FragmentError("conflicting fragment metadata")

        prior = assembly.fragments.get(fragment.fragment_index)
        if prior is not None:
            if prior != fragment.payload:
                del self._assemblies[key]
                raise FragmentError("conflicting duplicate fragment")
            return None
        assembly.fragments[fragment.fragment_index] = fragment.payload
        assembly.updated_at = instant
        if len(assembly.fragments) != assembly.fragment_count:
            return None

        payload = b"".join(
            assembly.fragments[index]
            for index in range(assembly.fragment_count)
        )
        del self._assemblies[key]
        if len(payload) != assembly.message_length:
            raise FragmentError("reassembled message length mismatch")
        if zlib.crc32(payload) & 0xFFFFFFFF != assembly.message_crc32:
            raise FragmentError("reassembled message CRC mismatch")
        if len(self._completed) >= self.max_completed:
            oldest = min(
                self._completed, key=lambda item: self._completed[item][1]
            )
            del self._completed[oldest]
        self._completed[key] = (assembly.metadata(), instant)
        return ReassembledTelemetry(
            message_type=assembly.message_type,
            source_id=assembly.source_id,
            mission_id=assembly.mission_id,
            message_sequence=assembly.message_sequence,
            flags=assembly.flags,
            fragment_count=assembly.fragment_count,
            payload=payload,
        )


@dataclass(frozen=True)
class TelemetryAcknowledgement:
    message_type: int
    source_id: int
    mission_id: int
    message_sequence: int
    message_crc32: int
    status: int = 0


def encode_acknowledgement(message: ReassembledTelemetry, *, status: int = 0) -> bytes:
    """Build a DTA1 cloud acknowledgement for a validated logical message."""
    status = _uint("status", status, 0xFFFF)
    message_crc = zlib.crc32(message.payload) & 0xFFFFFFFF
    without_crc = _ACK.pack(
        ACK_MAGIC, ACK_VERSION, message.message_type, ACK_WIRE_SIZE,
        message.source_id, message.mission_id, message.message_sequence,
        message_crc, status, 0, 0,
    )[:-4]
    return without_crc + _CRC.pack(zlib.crc32(without_crc) & 0xFFFFFFFF)


def decode_acknowledgement(encoded: bytes) -> TelemetryAcknowledgement:
    """Validate one fixed-size DTA1 acknowledgement."""
    encoded = _copy_bytes(encoded, "encoded acknowledgement")
    if len(encoded) != ACK_WIRE_SIZE:
        raise FragmentError("invalid acknowledgement length")
    (magic, version, message_type, size, source_id, mission_id, sequence,
     message_crc, status, reserved, stored_crc) = _ACK.unpack(encoded)
    if zlib.crc32(encoded[:-4]) & 0xFFFFFFFF != stored_crc:
        raise FragmentError("acknowledgement CRC mismatch")
    if (magic != ACK_MAGIC or version != ACK_VERSION or size != ACK_WIRE_SIZE or
            message_type == 0 or source_id == 0 or mission_id == 0 or
            reserved != 0):
        raise FragmentError("invalid acknowledgement metadata")
    return TelemetryAcknowledgement(
        message_type, source_id, mission_id, sequence, message_crc, status
    )
