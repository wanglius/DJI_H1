import struct
import unittest
import zlib

from record_decoder import (decode_file, decode_gps, FILE_MAGIC, RECORD_MAGIC,
                            RecordFormatError)


def make_record(record_type=2, sequence=1, body=b"payload"):
    size = 60 + len(body) + 4
    header = struct.pack(
        "<IHHIIIHHIQQQIHBB", RECORD_MAGIC, 1, record_type, 60, size,
        0x12340001, 2, 0, sequence, 1000, 2000, 3000, 4, 5, 2, 7)
    without_crc = header + body
    return without_crc + struct.pack("<I", zlib.crc32(without_crc) & 0xFFFFFFFF)


class RecordDecoderTests(unittest.TestCase):
    def setUp(self):
        self.header = struct.pack("<IHHII", FILE_MAGIC, 1, 2, 16, 0)

    def test_decodes_back_to_back_records(self):
        file_type, records = decode_file(
            self.header + make_record(sequence=1) + make_record(sequence=2))
        self.assertEqual(file_type, 2)
        self.assertEqual([record.sequence for record in records], [1, 2])
        self.assertEqual(records[0].body, b"payload")

    def test_rejects_bad_crc(self):
        damaged = bytearray(self.header + make_record())
        damaged[-5] ^= 0x80
        with self.assertRaisesRegex(RecordFormatError, "CRC mismatch"):
            decode_file(bytes(damaged))

    def test_rejects_truncated_record(self):
        with self.assertRaisesRegex(RecordFormatError, "truncated"):
            decode_file(self.header + make_record()[:-1])

    def test_rejects_record_type_mismatch(self):
        with self.assertRaisesRegex(RecordFormatError, "record header"):
            decode_file(self.header + make_record(record_type=3))

    def test_decodes_gps_track_body(self):
        body = struct.pack("<B3xiiiIIH8B", 0x5A, 399042000, 1164074000,
                           120000, 1767225600, 123456, 500,
                           3, 3, 50, 2, 15, 85, 0x1F, 0x0F)
        gps_header = struct.pack("<IHHII", FILE_MAGIC, 1, 1, 16, 0)
        _, records = decode_file(gps_header + make_record(
            record_type=1, body=body))
        gps = decode_gps(records[0])
        self.assertEqual(gps.protocol_sequence, 0x5A)
        self.assertEqual(gps.latitude_e7, 399042000)
        self.assertEqual(gps.utc_milliseconds, 500)
        self.assertEqual(gps.rtk_solution, 50)

    def test_rejects_wrong_gps_body_size(self):
        with self.assertRaisesRegex(RecordFormatError, "GPS record body"):
            decode_gps(decode_file(self.header + make_record())[1][0])


if __name__ == "__main__":
    unittest.main()
