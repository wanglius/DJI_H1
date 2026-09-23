import dataclasses
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_test import (AtParser, Modem, ReassembledTelemetry, ack_matches,
                      decode_acknowledgement, decode_record,
                      encode_acknowledgement, quote, spectrum)


class M100MTests(unittest.TestCase):
    def test_valid_full_spectrum_and_crc(self):
        payload = spectrum(1, 77)
        self.assertEqual(len(payload), 2233)
        record = decode_record(payload, expected_type=3, expected_sequence=1)
        self.assertEqual(record.info.sample_count, 711)
        self.assertEqual(record.header.session_id, 77)
        with self.assertRaises(ValueError):
            decode_record(payload[:-1] + bytes([payload[-1] ^ 1]))

    def test_binary_urc_all_split_boundaries(self):
        binary = b'\0\r\nOK\r\n>\r\n+MSUB: "fake",3 byte,abc\xff'
        wire = (b'\r\nPUBACK\r\n+MSUB: "test/down",' + str(len(binary)).encode()
                + b' byte,' + binary + b'\r\nOK\r\n> ')
        expected = [('line', 'PUBACK'), ('message', ('test/down', binary)),
                    ('line', 'OK'), ('line', '>')]
        for split in range(len(wire)+1):
            parser = AtParser()
            self.assertEqual(parser.feed(wire[:split])+parser.feed(wire[split:]), expected)
        parser = AtParser()
        result = []
        for b in wire:
            result.extend(parser.feed(bytes([b])))
        self.assertEqual(result, expected)

    def test_ack_requires_complete_identity_and_crc(self):
        payload = spectrum(3, 77)
        ack = decode_acknowledgement(encode_acknowledgement(
            ReassembledTelemetry(3, 11, 22, 3, 0, 1, payload)))
        self.assertTrue(ack_matches(ack, 11, 22, 3, payload))
        for name in ('source_id', 'mission_id', 'message_type',
                     'message_sequence', 'message_crc32'):
            wrong = dataclasses.replace(ack, **{name: getattr(ack, name)+1})
            self.assertFalse(ack_matches(wrong, 11, 22, 3, payload), name)

    def test_reject_oversize_and_at_injection(self):
        with self.assertRaises(ValueError):
            AtParser().feed(b'+MSUB: "x",999999 byte,')
        for text in ('a\r\nAT', 'a"b', 'a\\b'):
            with self.assertRaises(ValueError):
                quote(text)

    def test_puback_unblocks_modem_but_is_not_an_application_ack(self):
        from unittest.mock import Mock
        modem = Modem.__new__(Modem)
        modem.serial = Mock(in_waiting=8)
        modem.serial.read.return_value = b'\r\nPUBACK\r\n'
        modem.parser = AtParser()
        modem.journal = Mock()
        modem.message_handler = Mock()
        modem.lines = []
        modem.pubacks = 0
        modem.publish_busy = True
        modem.pump()
        self.assertFalse(modem.publish_busy)
        self.assertEqual(modem.pubacks, 1)
        modem.message_handler.assert_not_called()


if __name__ == '__main__':
    unittest.main()
