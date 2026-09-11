"""Minimal MQTT 3.1.1 publish/subscribe probe using only Python stdlib."""

from __future__ import annotations

import argparse
import socket
import time


def encode_length(value: int) -> bytes:
    encoded = bytearray()
    while True:
        digit = value % 128
        value //= 128
        if value:
            digit |= 0x80
        encoded.append(digit)
        if not value:
            return bytes(encoded)


def mqtt_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) > 0xFFFF:
        raise ValueError("MQTT string is too long")
    return len(encoded).to_bytes(2, "big") + encoded


def read_exact(connection: socket.socket, length: int) -> bytes:
    received = bytearray()
    while len(received) < length:
        chunk = connection.recv(length - len(received))
        if not chunk:
            raise ConnectionError("broker closed the MQTT connection")
        received.extend(chunk)
    return bytes(received)


def read_packet(connection: socket.socket) -> tuple[int, bytes]:
    first = read_exact(connection, 1)[0]
    multiplier = 1
    remaining = 0
    for _ in range(4):
        digit = read_exact(connection, 1)[0]
        remaining += (digit & 0x7F) * multiplier
        if not digit & 0x80:
            return first, read_exact(connection, remaining)
        multiplier *= 128
    raise ValueError("invalid MQTT remaining length")


def send_packet(connection: socket.socket, first: int, body: bytes) -> None:
    connection.sendall(bytes((first,)) + encode_length(len(body)) + body)


def connect_client(host: str, port: int, client_id: str,
                   username: str) -> socket.socket:
    connection = socket.create_connection((host, port), timeout=5.0)
    connection.settimeout(5.0)
    flags = 0x02  # Clean session.
    payload = mqtt_string(client_id)
    if username:
        flags |= 0x80
        payload += mqtt_string(username)
    variable = mqtt_string("MQTT") + bytes((4, flags, 0, 30))
    send_packet(connection, 0x10, variable + payload)
    first, body = read_packet(connection)
    if first != 0x20 or len(body) != 2 or body[1] != 0:
        connection.close()
        code = body[1] if len(body) >= 2 else None
        raise ConnectionError(f"MQTT CONNECT rejected: packet=0x{first:02x}, rc={code}")
    return connection


def subscribe(connection: socket.socket, topic: str, qos: int = 0) -> None:
    if qos not in (0, 1, 2):
        raise ValueError("subscription QoS must be 0, 1, or 2")
    packet_id = 1
    body = packet_id.to_bytes(2, "big") + mqtt_string(topic) + bytes((qos,))
    send_packet(connection, 0x82, body)
    first, response = read_packet(connection)
    if first != 0x90 or len(response) != 3 or response[:2] != body[:2]:
        raise ConnectionError("invalid MQTT SUBACK")
    if response[2] == 0x80:
        raise ConnectionError("broker rejected the MQTT subscription")


def publish(connection: socket.socket, topic: str, payload: bytes,
            qos: int = 0, packet_id: int = 1) -> None:
    if qos not in (0, 1):
        raise ValueError("the probe publisher supports QoS 0 or 1")
    body = mqtt_string(topic)
    if qos == 1:
        body += packet_id.to_bytes(2, "big")
    send_packet(connection, 0x30 | (qos << 1), body + payload)
    if qos == 1:
        first, response = read_packet(connection)
        if first != 0x40 or response != packet_id.to_bytes(2, "big"):
            raise ConnectionError("invalid MQTT PUBACK")


def receive_publish(connection: socket.socket) -> tuple[str, bytes, int]:
    while True:
        first, body = read_packet(connection)
        if first >> 4 != 3:
            continue
        if len(body) < 2:
            raise ValueError("truncated MQTT PUBLISH")
        topic_length = int.from_bytes(body[:2], "big")
        topic_end = 2 + topic_length
        if topic_end > len(body):
            raise ValueError("truncated MQTT PUBLISH topic")
        topic = body[2:topic_end].decode("utf-8")
        qos = (first >> 1) & 0x03
        payload_start = topic_end
        if qos:
            if payload_start + 2 > len(body):
                raise ValueError("truncated MQTT PUBLISH packet identifier")
            packet_id = body[payload_start:payload_start + 2]
            payload_start += 2
            if qos == 1:
                send_packet(connection, 0x40, packet_id)
            else:
                raise ValueError(f"the probe receiver does not support QoS {qos}")
        return topic, body[payload_start:], qos


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify anonymous MQTT 3.1.1 publish and subscribe")
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--username", default="")
    parser.add_argument("--topic", default="dji-h1/test/up")
    parser.add_argument("--qos", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()

    suffix = str(int(time.time() * 1000))[-8:]
    payload = f"DJI_H1 broker probe {suffix}".encode("ascii")
    subscriber = connect_client(
        args.host, args.port, f"DJI_H1_001_probe_sub_{suffix}", args.username)
    publisher = None
    try:
        subscribe(subscriber, args.topic, args.qos)
        publisher = connect_client(
            args.host, args.port, f"DJI_H1_001_probe_pub_{suffix}", args.username)
        publish(publisher, args.topic, payload, args.qos)
        deadline = time.monotonic() + 5.0
        while True:
            received_topic, received_payload, received_qos = receive_publish(subscriber)
            if (received_topic == args.topic and received_payload == payload and
                    received_qos == args.qos):
                break
            if time.monotonic() >= deadline:
                raise TimeoutError("matching MQTT probe message was not received")
        print(f"MQTT PASS host={args.host}:{args.port} topic={args.topic} "
              f"qos={args.qos}")
        print(f"payload={payload.decode('ascii')}")
    finally:
        for connection in (publisher, subscriber):
            if connection is not None:
                try:
                    send_packet(connection, 0xE0, b"")
                except OSError:
                    pass
                connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
