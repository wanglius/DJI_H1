from __future__ import annotations

from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from dji_h1_ground import (  # noqa: E402
    GpsSample, RecordFile, RecordFormatError, RECORD_GPS,
    RECORD_RAW_SPECTRUM,
)


FILE_MAGIC = 0x31464844
RECORD_MAGIC = 0x31524844
FILE_HEADER = struct.Struct("<IHHII")
RECORD_HEADER = struct.Struct("<IHHIIIHHIQQQIHBB")
RAW_PREFIX = struct.Struct("<IIHhBBBB")


def make_record(record_type: int, body: bytes, *, sequence: int = 1) -> bytes:
    size = RECORD_HEADER.size + len(body) + 4
    header = RECORD_HEADER.pack(
        RECORD_MAGIC, 1, record_type, RECORD_HEADER.size, size,
        0x12340001, 2, 0, sequence, 1000, 2000, 3000, 4, 5, 2, 7)
    without_crc = header + body
    return without_crc + struct.pack(
        "<I", zlib.crc32(without_crc) & 0xFFFFFFFF)


def raw_body(*samples: int) -> bytes:
    return (RAW_PREFIX.pack(7, 5000, len(samples), 0, 0, 0, 1, 0)
            + struct.pack(f"<{len(samples)}H", *samples))


class RecordDecoderTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self):
        self.temp_dir.cleanup()

    def write_file(self, record_type: int, records: bytes,
                   name: str = "records.bin") -> Path:
        path = self.root / name
        path.write_bytes(
            FILE_HEADER.pack(FILE_MAGIC, 1, record_type,
                             FILE_HEADER.size, 0) + records)
        return path

    def test_decodes_back_to_back_records(self):
        path = self.write_file(
            RECORD_RAW_SPECTRUM,
            make_record(RECORD_RAW_SPECTRUM, raw_body(10, 20), sequence=1)
            + make_record(
                RECORD_RAW_SPECTRUM, raw_body(30, 40), sequence=2))

        decoded = RecordFile.open(path)

        self.assertEqual(decoded.header.record_type, RECORD_RAW_SPECTRUM)
        self.assertEqual(
            [record.header.sequence for record in decoded.records], [1, 2])
        self.assertEqual(decoded.raw_spectrum(0).samples, (10, 20))

    def test_rejects_bad_crc(self):
        encoded = bytearray(make_record(
            RECORD_RAW_SPECTRUM, raw_body(10, 20)))
        encoded[-5] ^= 0x80
        path = self.write_file(RECORD_RAW_SPECTRUM, bytes(encoded))

        with self.assertRaisesRegex(RecordFormatError, "CRC mismatch"):
            RecordFile.open(path)

    def test_rejects_truncated_record(self):
        encoded = make_record(RECORD_RAW_SPECTRUM, raw_body(10, 20))[:-1]
        path = self.write_file(RECORD_RAW_SPECTRUM, encoded)

        with self.assertRaisesRegex(RecordFormatError, "truncated"):
            RecordFile.open(path)

    def test_rejects_record_type_mismatch(self):
        encoded = make_record(RECORD_RAW_SPECTRUM, raw_body(10, 20))
        path = self.write_file(RECORD_GPS, encoded)

        with self.assertRaisesRegex(RecordFormatError, "record header"):
            RecordFile.open(path)

    def test_decodes_gps_track_body(self):
        body = struct.pack(
            "<B3xiiiIIH8B", 0x5A, 399042000, 1164074000, 120000,
            1767225600, 123456, 500, 3, 3, 50, 2, 15, 85, 0x1F, 0x0F)
        path = self.write_file(
            RECORD_GPS, make_record(RECORD_GPS, body))

        decoded = RecordFile.open(path)
        header, gps = next(decoded.gps_samples())

        self.assertIsInstance(gps, GpsSample)
        self.assertEqual(header.sequence, 1)
        self.assertEqual(gps.protocol_sequence, 0x5A)
        self.assertEqual(gps.latitude_e7, 399042000)
        self.assertEqual(gps.utc_milliseconds, 500)
        self.assertEqual(gps.rtk_solution, 50)

    def test_rejects_wrong_gps_body_size(self):
        path = self.write_file(
            RECORD_GPS, make_record(RECORD_GPS, b"not-a-gps-body"))

        with self.assertRaisesRegex(RecordFormatError, "GPS body size"):
            RecordFile.open(path)


if __name__ == "__main__":
    unittest.main()
