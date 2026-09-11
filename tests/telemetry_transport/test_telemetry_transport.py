from __future__ import annotations

from pathlib import Path
import struct
import sys
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mission_viewer"))

from dji_h1_viewer.telemetry import (  # noqa: E402
    FRAGMENT_HEADER_SIZE, FRAGMENT_MAGIC, FRAGMENT_PAYLOAD_MAX,
    FRAGMENT_WIRE_MAX_SIZE, FragmentError, MESSAGE_GPS, MESSAGE_REFLECTANCE,
    TelemetryFragmentStreamDecoder, TelemetryReassembler,
    decode_acknowledgement, decode_fragment, encode_acknowledgement,
    fragment_message,
)


def _payload(length: int) -> bytes:
    return bytes((index * 37 + 11) & 0xFF for index in range(length))


def _validly_tamper_fragment(encoded: bytes, payload_offset: int = 0) -> bytes:
    damaged = bytearray(encoded)
    damaged[FRAGMENT_HEADER_SIZE + payload_offset] ^= 0x40
    damaged[-4:] = struct.pack("<I", zlib.crc32(damaged[:-4]) & 0xFFFFFFFF)
    return bytes(damaged)


class TelemetryTransportTests(unittest.TestCase):
    def test_gps_fits_one_fragment(self) -> None:
        payload = _payload(98)
        encoded, = fragment_message(MESSAGE_GPS, 7, 12, payload)
        self.assertEqual(len(encoded), FRAGMENT_HEADER_SIZE + 98 + 4)
        fragment = decode_fragment(encoded)
        self.assertEqual(fragment.fragment_index, 0)
        self.assertEqual(fragment.fragment_count, 1)
        self.assertEqual(fragment.payload, payload)
        self.assertEqual(fragment.source_id, 1)
        self.assertEqual(struct.unpack_from("<I", encoded)[0], FRAGMENT_MAGIC)

    def test_integer_is_not_treated_as_a_zero_filled_byte_buffer(self) -> None:
        with self.assertRaises(FragmentError):
            fragment_message(MESSAGE_GPS, 1, 1, 98)
        with self.assertRaises(FragmentError):
            decode_fragment(1024)
        with self.assertRaises(FragmentError):
            TelemetryFragmentStreamDecoder().feed(1024)

    def test_full_reflectance_uses_four_bounded_fragments(self) -> None:
        payload = _payload(3172)
        fragments = fragment_message(MESSAGE_REFLECTANCE, 42, 99, payload)
        self.assertEqual(len(fragments), 4)
        self.assertEqual([len(item) for item in fragments],
                         [1024, 1024, 1024, 292])
        self.assertTrue(all(len(item) <= FRAGMENT_WIRE_MAX_SIZE
                            for item in fragments))
        self.assertEqual(decode_fragment(fragments[0]).payload_length,
                         FRAGMENT_PAYLOAD_MAX)

    def test_reassembles_out_of_order_and_ignores_identical_duplicate(self) -> None:
        payload = _payload(3172)
        fragments = fragment_message(MESSAGE_REFLECTANCE, 42, 100, payload)
        reassembler = TelemetryReassembler()
        self.assertIsNone(reassembler.push(fragments[2]))
        self.assertIsNone(reassembler.push(fragments[0]))
        self.assertIsNone(reassembler.push(fragments[2]))
        self.assertIsNone(reassembler.push(fragments[3]))
        message = reassembler.push(fragments[1])
        self.assertIsNotNone(message)
        assert message is not None
        self.assertEqual(message.payload, payload)
        self.assertEqual(message.fragment_count, 4)
        self.assertEqual(message.message_sequence, 100)
        self.assertEqual(reassembler.inflight_count, 0)

        # A retransmission arriving after completion must not deliver the same
        # logical message a second time.
        for fragment in fragments:
            self.assertIsNone(reassembler.push(fragment))
        self.assertEqual(reassembler.inflight_count, 0)

    def test_source_identity_separates_same_mission_and_sequence(self) -> None:
        first = fragment_message(
            MESSAGE_GPS, 7, 12, _payload(98), source_id=0x111122223333
        )[0]
        second = fragment_message(
            MESSAGE_GPS, 7, 12, _payload(98), source_id=0x444455556666
        )[0]
        reassembler = TelemetryReassembler()
        one = reassembler.push(first)
        two = reassembler.push(second)
        self.assertEqual(one.source_id, 0x111122223333)
        self.assertEqual(two.source_id, 0x444455556666)

    def test_cloud_acknowledgement_round_trip_and_crc(self) -> None:
        fragment, = fragment_message(
            MESSAGE_GPS, 0xFEDCBA9876543210, 12, _payload(98),
            source_id=0x123456789ABC,
        )
        message = TelemetryReassembler().push(fragment)
        self.assertIsNotNone(message)
        encoded = encode_acknowledgement(message)
        ack = decode_acknowledgement(encoded)
        self.assertEqual(ack.source_id, 0x123456789ABC)
        self.assertEqual(ack.mission_id, 0xFEDCBA9876543210)
        self.assertEqual(ack.message_sequence, 12)
        damaged = bytearray(encoded)
        damaged[-1] ^= 1
        with self.assertRaisesRegex(FragmentError, "acknowledgement CRC"):
            decode_acknowledgement(damaged)

    def test_negative_cloud_acknowledgement_preserves_status(self) -> None:
        fragment, = fragment_message(MESSAGE_GPS, 7, 12, _payload(98))
        message = TelemetryReassembler().push(fragment)
        self.assertIsNotNone(message)
        assert message is not None
        encoded = encode_acknowledgement(message, status=7)
        self.assertEqual(decode_acknowledgement(encoded).status, 7)

    def test_rejects_corrupt_fragment_crc(self) -> None:
        fragment, = fragment_message(MESSAGE_GPS, 1, 2, _payload(98))
        damaged = bytearray(fragment)
        damaged[-5] ^= 0x80
        with self.assertRaisesRegex(FragmentError, "fragment CRC"):
            decode_fragment(damaged)

    def test_conflicting_duplicate_discards_assembly(self) -> None:
        fragments = fragment_message(
            MESSAGE_REFLECTANCE, 4, 5, _payload(2000)
        )
        reassembler = TelemetryReassembler()
        self.assertIsNone(reassembler.push(fragments[0]))
        with self.assertRaisesRegex(FragmentError, "conflicting duplicate"):
            reassembler.push(_validly_tamper_fragment(fragments[0]))
        self.assertEqual(reassembler.inflight_count, 0)

    def test_full_message_crc_detects_consistently_framed_damage(self) -> None:
        fragments = list(fragment_message(
            MESSAGE_REFLECTANCE, 4, 6, _payload(2000)
        ))
        fragments[0] = _validly_tamper_fragment(fragments[0])
        reassembler = TelemetryReassembler()
        self.assertIsNone(reassembler.push(fragments[0]))
        self.assertIsNone(reassembler.push(fragments[1]))
        with self.assertRaisesRegex(FragmentError, "message CRC"):
            reassembler.push(fragments[2])
        self.assertEqual(reassembler.inflight_count, 0)

    def test_expires_incomplete_message(self) -> None:
        now = [10.0]
        fragments = fragment_message(
            MESSAGE_REFLECTANCE, 8, 9, _payload(2000)
        )
        reassembler = TelemetryReassembler(
            timeout_seconds=5.0, clock=lambda: now[0]
        )
        self.assertIsNone(reassembler.push(fragments[0]))
        now[0] = 14.9
        self.assertEqual(reassembler.expire(), 0)
        now[0] = 15.0
        self.assertEqual(reassembler.expire(), 1)
        self.assertEqual(reassembler.inflight_count, 0)

    def test_bounds_inflight_messages(self) -> None:
        first = fragment_message(MESSAGE_REFLECTANCE, 1, 1, _payload(2000))
        second = fragment_message(MESSAGE_REFLECTANCE, 1, 2, _payload(2000))
        reassembler = TelemetryReassembler(max_inflight=1)
        self.assertIsNone(reassembler.push(first[0]))
        with self.assertRaisesRegex(FragmentError, "too many incomplete"):
            reassembler.push(second[0])

    def test_bounds_declared_message_size_at_receiver(self) -> None:
        fragment = fragment_message(
            MESSAGE_REFLECTANCE, 1, 3, _payload(2000)
        )[0]
        reassembler = TelemetryReassembler(max_message_size=1024)
        with self.assertRaisesRegex(FragmentError, "receiver limit"):
            reassembler.push(fragment)

    def test_stream_decoder_handles_garbage_splits_and_concatenation(self) -> None:
        payload = _payload(3172)
        wire = b"garbage" + b"".join(fragment_message(
            MESSAGE_REFLECTANCE, 10, 11, payload
        ))
        decoder = TelemetryFragmentStreamDecoder()
        reassembler = TelemetryReassembler()
        result = None
        for start in range(0, len(wire), 17):
            for fragment in decoder.feed(wire[start:start + 17]):
                completed = reassembler.push(fragment)
                if completed is not None:
                    result = completed
        self.assertIsNotNone(result)
        assert result is not None
        self.assertEqual(result.payload, payload)
        self.assertEqual(decoder.buffered_bytes, 0)

    def test_stream_decoder_resynchronizes_after_corrupt_fragment(self) -> None:
        bad, = fragment_message(MESSAGE_GPS, 1, 20, _payload(98))
        good, = fragment_message(MESSAGE_GPS, 1, 21, _payload(98))
        damaged = bytearray(bad)
        damaged[-1] ^= 0x80
        decoder = TelemetryFragmentStreamDecoder()
        decoded = decoder.feed(bytes(damaged) + good)
        self.assertEqual(len(decoded), 1)
        self.assertEqual(decoded[0].message_sequence, 21)


if __name__ == "__main__":
    unittest.main()
