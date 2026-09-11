"""Monitor and validate production DJI H1 telemetry from an MQTT broker."""

from __future__ import annotations

import argparse
from collections import OrderedDict
from pathlib import Path
import socket
import struct
import sys
import time
import zlib


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mission_viewer"))

from dji_h1_viewer.telemetry import (  # noqa: E402
    MESSAGE_GPS, MESSAGE_REFLECTANCE, FragmentError,
    TelemetryFragmentStreamDecoder, TelemetryReassembler,
    encode_acknowledgement,
)
from mqtt_broker_probe import (  # noqa: E402
    connect_client, publish, receive_publish, send_packet, subscribe,
)


RECORD_MAGIC = 0x31524844  # Little-endian DHR1.
RECORD_VERSION = 1
RECORD_HEADER_SIZE = 60
_RECORD_HEADER = struct.Struct("<IHHIIIHHIQQQIHBB")
_GPS_BODY = struct.Struct("<B3xiiiIIH8B")
_REFLECTANCE_PREFIX = struct.Struct("<IIIQIHHHHHH")
MAX_ACK_CACHE = 4096


def disconnect(connection) -> None:
    if connection is None:
        return
    try:
        send_packet(connection, 0xE0, b"")
    except OSError:
        pass
    connection.close()


def validate_record(payload: bytes, transport_type: int,
                    transport_sequence: int) -> tuple[tuple[int, ...], bytes]:
    if len(payload) < RECORD_HEADER_SIZE + 4:
        raise ValueError("truncated DHR1 record")
    fields = _RECORD_HEADER.unpack_from(payload)
    (magic, version, record_type, header_size, record_size, _session,
     _segment, _flags, record_sequence, *_time) = fields
    if magic != RECORD_MAGIC or version != RECORD_VERSION:
        raise ValueError("invalid DHR1 signature/version")
    if header_size != RECORD_HEADER_SIZE or record_size != len(payload):
        raise ValueError("invalid DHR1 declared size")
    if record_type != transport_type or record_sequence != transport_sequence:
        raise ValueError("DTF2/DHR1 metadata mismatch")
    expected_crc, = struct.unpack_from("<I", payload, len(payload) - 4)
    if zlib.crc32(payload[:-4]) & 0xFFFFFFFF != expected_crc:
        raise ValueError("DHR1 CRC mismatch")
    return fields, payload[RECORD_HEADER_SIZE:-4]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate live GPS and reflectance telemetry"
    )
    parser.add_argument("--host", required=True)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--username", default="")
    parser.add_argument("--topic", default="dji-h1/test/up")
    parser.add_argument("--ack-topic", default="dji-h1/test/down")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--expect-gps-min", type=int, default=0)
    parser.add_argument("--expect-reflectance-min", type=int, default=0)
    args = parser.parse_args()
    if args.duration <= 0:
        parser.error("--duration must be positive")

    suffix = str(int(time.time() * 1000))[-10:]
    connection = connect_client(
        args.host, args.mqtt_port,
        f"DJI_H1_monitor_{suffix}", args.username,
    )
    acknowledger = connect_client(
        args.host, args.mqtt_port,
        f"DJI_H1_ack_{suffix}", args.username,
    )
    stream = TelemetryFragmentStreamDecoder()
    reassembler = TelemetryReassembler(timeout_seconds=30.0)
    counts = {MESSAGE_GPS: 0, MESSAGE_REFLECTANCE: 0}
    last_sequences: dict[tuple[int, int, int], int] = {}
    source_records_skipped = {MESSAGE_GPS: 0, MESSAGE_REFLECTANCE: 0}
    # Retain enough recently completed identities to answer the ESP32's
    # bounded retransmission after a lost downlink ACK, without leaking memory
    # in a receiver service that may run for days.
    ack_cache: OrderedDict[tuple[int, ...], bytes] = OrderedDict()
    ack_packet_id = 1
    mqtt_messages = 0
    mqtt_bytes = 0
    try:
        subscribe(connection, args.topic, 1)
        connection.settimeout(1.0)
        deadline = time.monotonic() + args.duration
        while time.monotonic() < deadline:
            try:
                topic, mqtt_payload, qos = receive_publish(connection)
            except socket.timeout:
                reassembler.expire()
                continue
            if topic != args.topic:
                continue
            if qos != 1:
                raise RuntimeError(f"expected QoS 1, received QoS {qos}")
            mqtt_messages += 1
            mqtt_bytes += len(mqtt_payload)
            for fragment in stream.feed(mqtt_payload):
                key = (
                    fragment.source_id, fragment.mission_id,
                    fragment.message_type, fragment.message_sequence,
                    fragment.message_length, fragment.message_crc32,
                    fragment.fragment_count, fragment.flags,
                )
                cached_ack = ack_cache.get(key)
                if cached_ack is not None and fragment.fragment_index == 0:
                    ack_cache.move_to_end(key)
                    publish(acknowledger, args.ack_topic, cached_ack, 1,
                            ack_packet_id)
                    ack_packet_id = 1 if ack_packet_id == 0xFFFF else ack_packet_id + 1
                message = reassembler.push(fragment)
                if message is None:
                    continue
                if message.message_type not in counts:
                    raise ValueError(
                        f"unsupported telemetry type {message.message_type}"
                    )
                fields, body = validate_record(
                    message.payload, message.message_type,
                    message.message_sequence,
                )
                sequence_key = (message.source_id, message.mission_id,
                                message.message_type)
                previous = last_sequences.get(sequence_key)
                if previous is not None and message.message_sequence > previous + 1:
                    source_records_skipped[message.message_type] += (
                        message.message_sequence - previous - 1
                    )
                last_sequences[sequence_key] = message.message_sequence
                counts[message.message_type] = counts.get(message.message_type, 0) + 1
                utc_ms = fields[11]
                if message.message_type == MESSAGE_GPS:
                    if len(body) != _GPS_BODY.size:
                        raise ValueError("invalid GPS telemetry body size")
                    gps = _GPS_BODY.unpack(body)
                    validity = gps[-1]
                    position = (f"lat={gps[1] / 1e7:.7f} "
                                f"lon={gps[2] / 1e7:.7f}"
                                if validity & 0x01 else "position=INVALID")
                    print(
                        f"GPS source={message.source_id:012X} "
                        f"mission={message.mission_id} seq={message.message_sequence} "
                        f"utc_ms={utc_ms} {position} "
                        f"alt_m={gps[3] / 1000:.3f} valid=0x{validity:02X}"
                    )
                elif message.message_type == MESSAGE_REFLECTANCE:
                    if len(body) < _REFLECTANCE_PREFIX.size:
                        raise ValueError("truncated reflectance telemetry body")
                    reflectance = _REFLECTANCE_PREFIX.unpack_from(body)
                    sample_count = reflectance[5]
                    if len(body) != _REFLECTANCE_PREFIX.size + sample_count * 3:
                        raise ValueError("invalid reflectance telemetry body size")
                    print(
                        f"REFLECTANCE source={message.source_id:012X} "
                        f"mission={message.mission_id} "
                        f"seq={message.message_sequence} utc_ms={utc_ms} "
                        f"samples={sample_count} valid={reflectance[6]} "
                        f"sky_age_us={reflectance[4]}"
                    )
                encoded_ack = encode_acknowledgement(message)
                ack_cache[key] = encoded_ack
                ack_cache.move_to_end(key)
                if len(ack_cache) > MAX_ACK_CACHE:
                    ack_cache.popitem(last=False)
                publish(acknowledger, args.ack_topic, encoded_ack, 1,
                        ack_packet_id)
                ack_packet_id = 1 if ack_packet_id == 0xFFFF else ack_packet_id + 1
    except (FragmentError, ValueError, OSError, RuntimeError) as exc:
        print(f"TELEMETRY INVALID: {exc}", file=sys.stderr)
        return 2
    finally:
        disconnect(connection)
        disconnect(acknowledger)

    gps_count = counts[MESSAGE_GPS]
    reflectance_count = counts[MESSAGE_REFLECTANCE]
    print(
        "TELEMETRY SUMMARY "
        f"mqtt_messages={mqtt_messages} mqtt_bytes={mqtt_bytes} "
        f"gps={gps_count} reflectance={reflectance_count} "
        f"gps_source_records_skipped={source_records_skipped[MESSAGE_GPS]} "
        f"reflectance_source_records_skipped="
        f"{source_records_skipped[MESSAGE_REFLECTANCE]} "
        f"inflight={reassembler.inflight_count} buffered={stream.buffered_bytes}"
    )
    if gps_count < args.expect_gps_min or \
            reflectance_count < args.expect_reflectance_min:
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
