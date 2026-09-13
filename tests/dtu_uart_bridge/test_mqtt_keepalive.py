"""Regression tests for the lightweight MQTT clients' keepalive handling."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))

from monitor_telemetry import (  # noqa: E402
    PING_INTERVAL_SECONDS, service_keepalives,
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

    def test_ping_rejects_invalid_response(self) -> None:
        connection = FakeSocket(b"\x20\x02\x00\x00")

        with self.assertRaisesRegex(ConnectionError, "invalid MQTT PINGRESP"):
            ping(connection)


if __name__ == "__main__":
    unittest.main()
