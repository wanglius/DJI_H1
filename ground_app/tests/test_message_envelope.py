"""M100M single-publication transport, including the real ground worker path."""
import struct
import re
import zlib
from dataclasses import replace
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from dji_h1_ground.telemetry import (ReassembledTelemetry, encode_message,
    decode_message_envelope, decode_acknowledgement, FragmentError, fragment_message)
import test_ground_app as worker_tests
from test_ground_app import wait_for
from test_live_viewer import _reflectance


class MessageEnvelopeTests(unittest.TestCase):
    def test_documented_payload_consumer(self):
        doc = (Path(__file__).resolve().parents[1] / 'docs' / 'API使用说明_zh.md').read_text(encoding='utf-8')
        section = doc.split('## 3.', 1)[1].split('## 4.', 1)[0]
        example = re.search(r'```python\n(.*?)```', section, re.S).group(1)
        namespace = {}
        exec(compile(example, 'API使用说明_zh.md', 'exec'), namespace)
        consume = namespace['consume_payload']
        payload = _reflectance(1, 2050)
        wire = encode_message(ReassembledTelemetry(3, 11, 42, 1, 0, 1, payload))
        modern = consume(wire)
        legacy = []
        for fragment in fragment_message(3, 42, 1, payload, source_id=11):
            # Legacy transparent UART may split publications arbitrarily.
            legacy.extend(consume(fragment[:17]))
            legacy.extend(consume(fragment[17:]))
        self.assertEqual(len(modern), 1)
        self.assertEqual(len(legacy), 1)
        self.assertEqual(modern[0].records, legacy[0].records)
        self.assertEqual(modern[0].source_id, 11)
        bad = bytearray(wire)
        bad[-1] ^= 1
        with self.assertRaises(FragmentError):
            consume(bytes(bad))

    def test_c_golden_vector(self):
        wire = encode_message(ReassembledTelemetry(3, 0x123456789ABC, 42, 7, 0, 1, b'123456789'))
        self.assertEqual(wire.hex(), '44544d3101030000bc9a7856341200002a00000000000000'
                         '07000000090000002639f4cb31323334353637383998f472bc')

    def test_full_spectrum_and_corruption(self):
        msg = ReassembledTelemetry(3, 11, 42, 1, 0, 1, _reflectance(1, 2050))
        wire = encode_message(msg)
        self.assertEqual(len(wire), len(msg.payload) + 40)
        self.assertEqual(decode_message_envelope(wire), msg)
        for index in (0, 4, 5, 6, 8, 16, 24, 28, 32, 36, len(wire)-1):
            bad = bytearray(wire); bad[index] ^= 1
            with self.assertRaises(FragmentError): decode_message_envelope(bad)
        for bad in (wire[:-1], wire + b'x', wire + wire):
            with self.assertRaises(FragmentError): decode_message_envelope(bad)

    def test_payload_crc_even_with_valid_wire_crc(self):
        wire = bytearray(encode_message(ReassembledTelemetry(3, 11, 42, 1, 0, 1, b'123')))
        wire[36] ^= 1
        wire[-4:] = struct.pack('<I', zlib.crc32(wire[:-4]) & 0xFFFFFFFF)
        with self.assertRaises(FragmentError): decode_message_envelope(wire)


class SingleMessageWorkerTests(worker_tests.GroundAppTests):
    # Run all existing journal/filter/retry/API tests again through DTM1.
    def feed(self, obj, kind=3, seq=1, data=None):
        wire = encode_message(ReassembledTelemetry(kind, 11, 42, seq, 0, 1,
                              data or _reflectance(seq, 2050)))
        obj._enqueue_publication(obj.config.uplink_topic, wire, 1)

    def test_duplicate_validated_without_reassembly(self):
        obj = self.receiver()
        self.feed(obj); self.feed(obj)
        wait_for(lambda: obj.status()['acknowledgements'] == 2)
        self.assertEqual(obj.status()['decoded_messages'], 1)
        self.assertEqual(obj.status()['inflight'], 0)
        self.assertEqual(obj.status()['buffered_bytes'], 0)
        ack = decode_acknowledgement(obj._transport.publish.call_args.args[1])
        self.assertEqual((ack.source_id, ack.mission_id, ack.message_sequence), (11, 42, 1))
