"""Regression tests for the lightweight MQTT clients' keepalive handling."""

from __future__ import annotations

from pathlib import Path
import struct
import sys
import unittest


THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))

from monitor_telemetry import (  # noqa: E402
    DEFAULT_ACK_QOS, PING_INTERVAL_SECONDS, decode_operation_event,
    publish_ack, service_keepalives,
)
from mqtt_broker_probe import ping  # noqa: E402


class FakeSocket:
    def __init__(self, received: bytes = b"") -> None:
        self.received = bytearray(received)
        self.sent = bytearray()

    def sendall(self, payload: bytes) -> None:
        self.sent.extend(payload)

    def recv(self, length: int) -> bytes:
        if not self.received:
            return b""
        chunk = self.received[:length]
        del self.received[:length]
        return bytes(chunk)


class KeepaliveTests(unittest.TestCase):
    def test_application_ack_defaults_to_nonblocking_qos_zero(self) -> None:
        connection = FakeSocket()

        next_packet_id = publish_ack(
            connection, "dji-h1/test/down", b"DTA1", DEFAULT_ACK_QOS, 7,
        )

        self.assertEqual(DEFAULT_ACK_QOS, 0)
        self.assertEqual(connection.sent[0], 0x30)
        self.assertTrue(connection.sent.endswith(b"DTA1"))
        self.assertEqual(next_packet_id, 7)

    def test_diagnostic_qos_one_ack_waits_for_matching_puback(self) -> None:
        connection = FakeSocket(b"\x40\x02\x00\x07")

        next_packet_id = publish_ack(
            connection, "dji-h1/test/down", b"DTA1", 1, 7,
        )

        self.assertEqual(connection.sent[0], 0x32)
        self.assertEqual(connection.received, b"")
        self.assertEqual(next_packet_id, 8)

    def test_ping_validates_and_consumes_pingresp(self) -> None:
        connection = FakeSocket(b"\xD0\x00")

        ping(connection)

        self.assertEqual(connection.sent, b"\xC0\x00")
        self.assertEqual(connection.received, b"")

    def test_service_runs_on_elapsed_time_not_receive_timeout(self) -> None:
        subscriber = FakeSocket()
        acknowledger = FakeSocket(b"\xD0\x00")
        last_ping = 100.0

        unchanged = service_keepalives(
            subscriber, acknowledger, last_ping,
            last_ping + PING_INTERVAL_SECONDS - 0.001,
        )
        self.assertEqual(unchanged, last_ping)
        self.assertEqual(subscriber.sent, b"")
        self.assertEqual(acknowledger.sent, b"")

        now = last_ping + PING_INTERVAL_SECONDS
        updated = service_keepalives(
            subscriber, acknowledger, last_ping, now,
        )
        self.assertEqual(updated, now)
        self.assertEqual(subscriber.sent, b"\xC0\x00")
        self.assertEqual(acknowledger.sent, b"\xC0\x00")

    def test_service_supports_fire_and_forget_without_ack_client(self) -> None:
        subscriber = FakeSocket()
        now = 110.0

        updated = service_keepalives(subscriber, None, 100.0, now)

        self.assertEqual(updated, now)
        self.assertEqual(subscriber.sent, b"\xC0\x00")

    def test_ping_rejects_invalid_response(self) -> None:
        connection = FakeSocket(b"\x20\x02\x00\x00")

        with self.assertRaisesRegex(ConnectionError, "invalid MQTT PINGRESP"):
            ping(connection)

    def test_operation_event_body_is_validated(self) -> None:
        body = struct.pack("<HBBIi", 13, 3, 0, 3000, -1)
        self.assertEqual(decode_operation_event(body), (13, 3, 3000, -1))
        with self.assertRaisesRegex(ValueError, "operation-event body"):
            decode_operation_event(struct.pack("<HBBIi", 13, 4, 0, 0, 0))


if __name__ == "__main__":
    unittest.main()
