"""Exercise DTF2 fragmentation and reassembly through the real DTU/broker."""

from __future__ import annotations

import argparse
from pathlib import Path
import socket
import sys
import threading
import time

import serial


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mission_viewer"))

from dji_h1_viewer.telemetry import (  # noqa: E402
    MESSAGE_REFLECTANCE, TelemetryFragmentStreamDecoder,
    TelemetryReassembler, fragment_message,
)
from mqtt_broker_probe import (  # noqa: E402
    connect_client, receive_publish, send_packet, subscribe,
)


def disconnect(connection) -> None:
    if connection is None:
        return
    try:
        send_packet(connection, 0xE0, b"")
    except OSError:
        pass
    connection.close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify fragmented telemetry through the DTU MQTT uplink"
    )
    parser.add_argument("--serial-port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--host", required=True)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--username", default="")
    parser.add_argument("--topic", default="dji-h1/test/up")
    parser.add_argument("--expect-qos", type=int, choices=(0, 1), default=0)
    parser.add_argument("--source-id", type=lambda value: int(value, 0),
                        default=0x12345678)
    parser.add_argument("--message-bytes", type=int, default=3172)
    parser.add_argument("--message-count", type=int, default=1)
    parser.add_argument("--fragment-gap-ms", type=float, default=50.0)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if args.message_bytes < 0:
        parser.error("--message-bytes must not be negative")
    if args.message_count <= 0:
        parser.error("--message-count must be positive")
    if args.fragment_gap_ms < 0:
        parser.error("--fragment-gap-ms must not be negative")
    if not 1 <= args.source_id <= 0xFFFFFFFFFFFFFFFF:
        parser.error("--source-id must be a nonzero uint64")

    suffix = str(int(time.time() * 1000))[-10:]
    first_sequence = int(suffix) & 0xFFFFFFFF
    expected: dict[int, bytes] = {}
    messages: list[tuple[bytes, ...]] = []
    for message_index in range(args.message_count):
        sequence = (first_sequence + message_index) & 0xFFFFFFFF
        payload = bytes(
            (index * 37 + 11 + message_index) & 0xFF
            for index in range(args.message_bytes)
        )
        expected[sequence] = payload
        messages.append(fragment_message(
            MESSAGE_REFLECTANCE, 0xF001, sequence, payload,
            source_id=args.source_id,
        ))
    subscriber = None
    try:
        subscriber = connect_client(
            args.host, args.mqtt_port,
            f"DJI_H1_fragment_sub_{suffix}", args.username,
        )
        subscribe(subscriber, args.topic, 1)
        subscriber.settimeout(1.0)

        stream_decoder = TelemetryFragmentStreamDecoder()
        reassembler = TelemetryReassembler(timeout_seconds=args.timeout)
        mqtt_sizes: list[int] = []
        decoded_fragments = 0
        completed: dict[int, bytes] = {}
        receiver_errors: list[BaseException] = []
        deadline = time.monotonic() + args.timeout

        def receive_worker() -> None:
            nonlocal decoded_fragments
            try:
                while (len(completed) < len(expected) and
                       time.monotonic() < deadline):
                    try:
                        topic, mqtt_payload, qos = receive_publish(subscriber)
                    except socket.timeout:
                        continue
                    if topic != args.topic:
                        continue
                    if qos != args.expect_qos:
                        raise RuntimeError(
                            f"expected QoS {args.expect_qos}, received QoS {qos}"
                        )
                    mqtt_sizes.append(len(mqtt_payload))
                    for fragment in stream_decoder.feed(mqtt_payload):
                        decoded_fragments += 1
                        result = reassembler.push(fragment)
                        if result is None:
                            continue
                        wanted = expected.get(result.message_sequence)
                        if wanted is None:
                            raise RuntimeError(
                                "received an unexpected message sequence"
                            )
                        if result.message_sequence in completed:
                            raise RuntimeError(
                                "logical message was delivered twice"
                            )
                        if result.payload != wanted:
                            raise RuntimeError(
                                "reassembled payload differs from source"
                            )
                        completed[result.message_sequence] = result.payload
            except BaseException as exc:  # Propagate worker failures to main.
                receiver_errors.append(exc)

        receiver = threading.Thread(
            target=receive_worker, name="mqtt-fragment-receiver", daemon=True
        )
        receiver.start()
        with serial.Serial(args.serial_port, args.baud, timeout=0.1) as port:
            port.dtr = False
            port.rts = False
            time.sleep(0.5)
            port.reset_input_buffer()
            for application_fragments in messages:
                for fragment in application_fragments:
                    if port.write(fragment) != len(fragment):
                        raise IOError("short serial write")
                    port.flush()
                    if args.fragment_gap_ms:
                        time.sleep(args.fragment_gap_ms / 1000.0)

        receiver.join(max(0.0, deadline - time.monotonic()) + 2.0)
        if receiver_errors:
            raise receiver_errors[0]
        if receiver.is_alive():
            raise TimeoutError("MQTT receiver did not stop at its deadline")
        if len(completed) != len(expected):
            missing = sorted(set(expected) - set(completed))
            print(
                f"DIAGNOSTIC mqtt_messages={len(mqtt_sizes)} "
                f"mqtt_sizes={mqtt_sizes} decoded_fragments={decoded_fragments} "
                f"completed={len(completed)} "
                f"inflight={reassembler.inflight_count} "
                f"stream_buffered={stream_decoder.buffered_bytes}"
            )
            raise TimeoutError(
                f"fragmented telemetry did not reassemble; missing={missing}"
            )

        application_sizes = [len(item) for item in messages[0]]
        print(
            f"FRAGMENTED TELEMETRY PASS qos={args.expect_qos} "
            f"messages={len(completed)} "
            f"source={args.source_id:012X} "
            f"bytes_each={args.message_bytes} "
            f"fragments_each={len(messages[0])} "
            f"application_sizes={application_sizes}"
        )
        print(
            f"mqtt_messages={len(mqtt_sizes)} mqtt_sizes={mqtt_sizes} "
            f"decoded_fragments={decoded_fragments}"
        )
    finally:
        disconnect(subscriber)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
