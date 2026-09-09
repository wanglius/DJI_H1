"""Indexed, CRC-validating decoder for DJI H1 measurement record format v01."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct
from typing import Iterator
import zlib


FILE_MAGIC = 0x31464844  # DHF1
RECORD_MAGIC = 0x31524844  # DHR1
FORMAT_VERSION = 1
FILE_HEADER_SIZE = 16
RECORD_HEADER_SIZE = 60
# v01 H1 records are only a few kilobytes. This generous ceiling prevents a
# damaged size field from causing an unbounded allocation during the CRC scan.
MAX_RECORD_SIZE = 4 * 1024 * 1024

RECORD_GPS = 1
RECORD_RAW_SPECTRUM = 2
RECORD_REFLECTANCE = 3

ROLE_GROUND = 0
ROLE_SKY = 1

_FILE_HEADER = struct.Struct("<IHHII")
_RECORD_HEADER = struct.Struct("<IHHIIIHHIQQQIHBB")
_RAW_PREFIX = struct.Struct("<IIHhBBBB")
_REFLECTANCE_PREFIX = struct.Struct("<IIIQIHHHHHH")
_GPS_BODY = struct.Struct("<B3xiiiIIH8B")


class RecordFormatError(ValueError):
    """The file is not a complete, valid DJI H1 v01 record stream."""


@dataclass(frozen=True)
class FileHeader:
    record_type: int
    format_version: int = FORMAT_VERSION


@dataclass(frozen=True)
class RecordHeader:
    record_type: int
    record_size: int
    session_id: int
    segment_id: int
    flags: int
    sequence: int
    b_monotonic_us: int
    a_monotonic_ms: int
    utc_ms: int
    sync_age_ms: int
    sync_generation: int
    sync_state: int
    time_valid_flags: int


@dataclass(frozen=True)
class RawRecordInfo:
    frame_count: int
    exposure_us: int
    sample_count: int
    spectrum_scale: int
    spectrometer_role: int
    exposure_status: int
    frame_quality: int


@dataclass(frozen=True)
class ReflectanceRecordInfo:
    calculation_count: int
    ground_frame_count: int
    sky_frame_count: int
    sky_b_monotonic_us: int
    sky_age_us: int
    sample_count: int
    valid_sample_count: int
    clamped_low_count: int
    clamped_high_count: int
    invalid_denominator_count: int
    input_quality_flags: int


@dataclass(frozen=True)
class GpsSample:
    protocol_sequence: int
    latitude_e7: int
    longitude_e7: int
    altitude_relative_mm: int
    utc_seconds: int
    a_monotonic_ms: int
    utc_milliseconds: int
    source_flags: int
    gps_fix: int
    rtk_solution: int
    flight_status: int
    display_mode: int
    battery_percent: int
    a_status: int
    valid_flags: int


@dataclass(frozen=True)
class RecordRef:
    """A lightweight record index entry; sample arrays remain on disk."""

    header: RecordHeader
    file_offset: int
    body_offset: int
    body_size: int
    info: RawRecordInfo | ReflectanceRecordInfo | GpsSample | None
    expected_crc: int


@dataclass(frozen=True)
class RawSpectrum:
    header: RecordHeader
    info: RawRecordInfo
    samples: tuple[int, ...]


@dataclass(frozen=True)
class ReflectanceSpectrum:
    header: RecordHeader
    info: ReflectanceRecordInfo
    reflectance_0p01_percent: tuple[int, ...]
    sample_flags: bytes


def _record_info(record_type: int, body: bytes, offset: int):
    if record_type == RECORD_RAW_SPECTRUM:
        if len(body) < _RAW_PREFIX.size:
            raise RecordFormatError(f"truncated raw body at {offset}")
        fields = _RAW_PREFIX.unpack_from(body)
        info = RawRecordInfo(*fields[:-1])
        if fields[-1] != 0 or len(body) != _RAW_PREFIX.size + info.sample_count * 2:
            raise RecordFormatError(f"invalid raw body size at {offset}")
        return info
    if record_type == RECORD_REFLECTANCE:
        if len(body) < _REFLECTANCE_PREFIX.size:
            raise RecordFormatError(f"truncated reflectance body at {offset}")
        info = ReflectanceRecordInfo(*_REFLECTANCE_PREFIX.unpack_from(body))
        if len(body) != _REFLECTANCE_PREFIX.size + info.sample_count * 3:
            raise RecordFormatError(f"invalid reflectance body size at {offset}")
        return info
    if record_type == RECORD_GPS:
        if len(body) != _GPS_BODY.size:
            raise RecordFormatError(f"invalid GPS body size at {offset}")
        return GpsSample(*_GPS_BODY.unpack(body))
    return None


class RecordFile:
    """A scanned record file with lazy access to large sample arrays.

    ``open`` performs one sequential pass and optionally verifies every CRC.
    Only headers and compact type-specific metadata stay in memory.
    """

    def __init__(self, path: Path, header: FileHeader,
                 records: tuple[RecordRef, ...], crc_verified: bool):
        self.path = path
        self.header = header
        self.records = records
        self.crc_verified = crc_verified

    @classmethod
    def open(cls, path: str | Path, *, verify_crc: bool = True) -> "RecordFile":
        source = Path(path)
        records: list[RecordRef] = []
        with source.open("rb") as stream:
            file_bytes = stream.read(FILE_HEADER_SIZE)
            if len(file_bytes) != FILE_HEADER_SIZE:
                raise RecordFormatError("truncated file header")
            magic, version, record_type, header_size, reserved = \
                _FILE_HEADER.unpack(file_bytes)
            if magic != FILE_MAGIC or version != FORMAT_VERSION:
                raise RecordFormatError("unsupported file signature/version")
            if header_size != FILE_HEADER_SIZE or reserved != 0:
                raise RecordFormatError("invalid file header")

            offset = FILE_HEADER_SIZE
            while True:
                encoded_header = stream.read(RECORD_HEADER_SIZE)
                if not encoded_header:
                    break
                if len(encoded_header) != RECORD_HEADER_SIZE:
                    raise RecordFormatError(f"truncated record header at {offset}")
                fields = _RECORD_HEADER.unpack(encoded_header)
                (record_magic, record_version, item_type, item_header_size,
                 record_size, session_id, segment_id, flags, sequence,
                 b_us, a_ms, utc_ms, sync_age, sync_generation, sync_state,
                 valid_flags) = fields
                if record_magic != RECORD_MAGIC or record_version != FORMAT_VERSION:
                    raise RecordFormatError(
                        f"invalid record signature/version at {offset}")
                if item_header_size != RECORD_HEADER_SIZE or item_type != record_type:
                    raise RecordFormatError(f"invalid record header at {offset}")
                if record_size > MAX_RECORD_SIZE:
                    raise RecordFormatError(f"record is unreasonably large at {offset}")
                body_size = record_size - RECORD_HEADER_SIZE - 4
                if body_size < 0:
                    raise RecordFormatError(f"invalid record size at {offset}")
                body = stream.read(body_size)
                crc_bytes = stream.read(4)
                if len(body) != body_size or len(crc_bytes) != 4:
                    raise RecordFormatError(f"truncated record at {offset}")
                expected_crc, = struct.unpack("<I", crc_bytes)
                if verify_crc:
                    actual_crc = zlib.crc32(encoded_header)
                    actual_crc = zlib.crc32(body, actual_crc) & 0xFFFFFFFF
                    if actual_crc != expected_crc:
                        raise RecordFormatError(f"CRC mismatch at {offset}")
                header = RecordHeader(
                    item_type, record_size, session_id, segment_id, flags,
                    sequence, b_us, a_ms, utc_ms, sync_age, sync_generation,
                    sync_state, valid_flags)
                records.append(RecordRef(
                    header, offset, offset + RECORD_HEADER_SIZE, body_size,
                    _record_info(item_type, body, offset), expected_crc))
                offset += record_size
        return cls(source, FileHeader(record_type, version), tuple(records),
                   verify_crc)

    def __len__(self) -> int:
        return len(self.records)

    def read_body(self, index: int) -> bytes:
        ref = self.records[index]
        with self.path.open("rb") as stream:
            stream.seek(ref.file_offset)
            encoded = stream.read(ref.header.record_size)
        if len(encoded) != ref.header.record_size:
            raise RecordFormatError(f"record changed or truncated at {ref.file_offset}")
        encoded_header = encoded[:RECORD_HEADER_SIZE]
        body_end = RECORD_HEADER_SIZE + ref.body_size
        body = encoded[RECORD_HEADER_SIZE:body_end]
        stored_crc, = struct.unpack_from("<I", encoded, body_end)
        if self.crc_verified:
            actual_crc = zlib.crc32(encoded_header)
            actual_crc = zlib.crc32(body, actual_crc) & 0xFFFFFFFF
            # Comparing with the scan-time CRC also detects a valid record file
            # being replaced underneath the immutable in-memory index.
            if stored_crc != ref.expected_crc or actual_crc != stored_crc:
                raise RecordFormatError(
                    f"record changed or CRC mismatch at {ref.file_offset}")
        return body

    def raw_spectrum(self, index: int) -> RawSpectrum:
        ref = self.records[index]
        if not isinstance(ref.info, RawRecordInfo):
            raise TypeError("record file does not contain raw spectra")
        body = self.read_body(index)
        samples = struct.unpack_from(
            f"<{ref.info.sample_count}H", body, _RAW_PREFIX.size)
        return RawSpectrum(ref.header, ref.info, samples)

    def reflectance_spectrum(self, index: int) -> ReflectanceSpectrum:
        ref = self.records[index]
        if not isinstance(ref.info, ReflectanceRecordInfo):
            raise TypeError("record file does not contain reflectance spectra")
        body = self.read_body(index)
        count = ref.info.sample_count
        values = struct.unpack_from(f"<{count}H", body, _REFLECTANCE_PREFIX.size)
        flags_offset = _REFLECTANCE_PREFIX.size + count * 2
        return ReflectanceSpectrum(
            ref.header, ref.info, values, body[flags_offset:flags_offset + count])

    def gps_samples(self) -> Iterator[tuple[RecordHeader, GpsSample]]:
        for ref in self.records:
            if not isinstance(ref.info, GpsSample):
                raise TypeError("record file does not contain GPS samples")
            yield ref.header, ref.info
