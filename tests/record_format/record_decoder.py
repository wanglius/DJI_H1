"""Reference reader for DJI_H1 v01 binary measurement files."""
from dataclasses import dataclass
from pathlib import Path
import struct
import zlib

FILE_MAGIC = 0x31464844
RECORD_MAGIC = 0x31524844
FORMAT_VERSION = 1
FILE_HEADER_SIZE = 16
RECORD_HEADER_SIZE = 60


class RecordFormatError(ValueError):
    pass


@dataclass(frozen=True)
class Record:
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
    valid_flags: int
    body: bytes


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


def decode_file(data: bytes) -> tuple[int, list[Record]]:
    """Decode and CRC-check a complete file, rejecting truncated tails."""
    if len(data) < FILE_HEADER_SIZE:
        raise RecordFormatError("truncated file header")
    magic, version, file_type, header_size, reserved = struct.unpack_from(
        "<IHHII", data)
    if magic != FILE_MAGIC or version != FORMAT_VERSION:
        raise RecordFormatError("unsupported file signature/version")
    if header_size != FILE_HEADER_SIZE or reserved != 0:
        raise RecordFormatError("invalid file header")

    records = []
    offset = header_size
    while offset < len(data):
        if len(data) - offset < RECORD_HEADER_SIZE:
            raise RecordFormatError(f"truncated record header at {offset}")
        fields = struct.unpack_from("<IHHIIIHHIQQQIHBB", data, offset)
        (record_magic, record_version, record_type, record_header_size,
         record_size, session_id, segment_id, flags, sequence,
         b_us, a_ms, utc_ms, sync_age, sync_generation, sync_state,
         valid_flags) = fields
        if record_magic != RECORD_MAGIC or record_version != FORMAT_VERSION:
            raise RecordFormatError(f"invalid record signature/version at {offset}")
        if record_header_size != RECORD_HEADER_SIZE or record_type != file_type:
            raise RecordFormatError(f"invalid record header at {offset}")
        if record_size < RECORD_HEADER_SIZE + 4 or offset + record_size > len(data):
            raise RecordFormatError(f"invalid/truncated record size at {offset}")
        encoded = data[offset:offset + record_size]
        expected_crc, = struct.unpack_from("<I", encoded, record_size - 4)
        actual_crc = zlib.crc32(encoded[:-4]) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise RecordFormatError(f"CRC mismatch at {offset}")
        records.append(Record(
            record_type, record_size, session_id, segment_id, flags, sequence,
            b_us, a_ms, utc_ms, sync_age, sync_generation, sync_state,
            valid_flags, encoded[RECORD_HEADER_SIZE:-4]))
        offset += record_size
    return file_type, records


def decode_path(path: str | Path) -> tuple[int, list[Record]]:
    return decode_file(Path(path).read_bytes())


def decode_gps(record: Record) -> GpsSample:
    """Decode the fixed 34-byte body of a v01 GPS_TRACK record."""
    if record.record_type != 1 or len(record.body) != 34:
        raise RecordFormatError("invalid GPS record body")
    fields = struct.unpack("<B3xiiiIIH8B", record.body)
    return GpsSample(*fields)
