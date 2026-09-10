"""Verify transparent DTU MQTT transport in both directions."""

from __future__ import annotations

import argparse
import time

import serial

from mqtt_broker_probe import (
    connect_client,
    publish,
    receive_publish,
    send_packet,
    subscribe,
)


def receive_matching_publish(
    connection, topic: str, payload: bytes, timeout_seconds: float,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        received_topic, received_payload = receive_publish(connection)
        if received_topic == topic and received_payload == payload:
            return
    raise TimeoutError("DTU uplink payload was not received from the broker")


def receive_serial_payload(
    port: serial.Serial, payload: bytes, timeout_seconds: float,
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if chunk:
            received.extend(chunk)
            if payload in received:
                return bytes(received)
    raise TimeoutError("DTU downlink payload was not received on the serial port")


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
        description="Verify PC -> DTU -> broker and broker -> DTU -> PC"
    )
    parser.add_argument("--serial-port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--host", required=True)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--username", default="")
    parser.add_argument("--up-topic", default="dji-h1/test/up")
    parser.add_argument("--down-topic", default="dji-h1/test/down")
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    suffix = str(int(time.time() * 1000))[-10:]
    uplink_payload = f"DJI_H1_DTU_UP_{suffix}".encode("ascii")
    downlink_payload = f"DJI_H1_DTU_DOWN_{suffix}".encode("ascii")
    subscriber = None
    publisher = None

    try:
        subscriber = connect_client(
            args.host, args.mqtt_port,
            f"DJI_H1_path_sub_{suffix}", args.username,
        )
        subscribe(subscriber, args.up_topic)
        publisher = connect_client(
            args.host, args.mqtt_port,
            f"DJI_H1_path_pub_{suffix}", args.username,
        )

        with serial.Serial(args.serial_port, args.baud, timeout=0.1) as port:
            port.dtr = False
            port.rts = False
            time.sleep(0.5)
            port.reset_input_buffer()

            port.write(uplink_payload)
            port.flush()
            receive_matching_publish(
                subscriber, args.up_topic, uplink_payload, args.timeout
            )
            print(
                f"UPLINK PASS serial={args.serial_port} -> "
                f"topic={args.up_topic} payload={uplink_payload.decode('ascii')}"
            )

            port.reset_input_buffer()
            publish(publisher, args.down_topic, downlink_payload)
            receive_serial_payload(port, downlink_payload, args.timeout)
            print(
                f"DOWNLINK PASS topic={args.down_topic} -> "
                f"serial={args.serial_port} payload={downlink_payload.decode('ascii')}"
            )
    finally:
        disconnect(publisher)
        disconnect(subscriber)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
