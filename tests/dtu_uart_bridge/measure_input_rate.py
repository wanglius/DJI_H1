"""Measure sustainable DTU UART-to-MQTT input rates with controlled loads.

The ESP32 must be running the transparent DTU UART bridge firmware.  This
probe deliberately sends synthetic payloads: their wire sizes and DTF2
fragmentation match production DGB1 GPS batches and v01 reflectance records, while keeping
the measurement independent of acquisition, SD-card, and application-ACK
behavior.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import socket
import struct
import sys
import threading
import time

THIS_DIR = Path(__file__).resolve().parent
ROOT = THIS_DIR.parents[1]
sys.path.insert(0, str(ROOT / "tools" / "mission_viewer"))
sys.path.insert(0, str(THIS_DIR))

from dji_h1_viewer.telemetry import (  # noqa: E402
    MESSAGE_GPS_BATCH,
    MESSAGE_REFLECTANCE,
    TelemetryFragmentStreamDecoder,
    TelemetryReassembler,
    fragment_message,
)
from mqtt_broker_probe import (  # noqa: E402
    connect_client,
    ping,
    receive_publish,
    send_packet,
    subscribe,
)


# Exact production DGB1 maximum: 16-byte batch header, one shared 24-byte DHR1
# prefix, ten 70-byte record suffixes, and a four-byte batch CRC. The payload
# is synthetic because this test isolates transparent-link capacity.
GPS_PAYLOAD_BYTES = 16 + 24 + 10 * 70 + 4
DEFAULT_REFLECTANCE_SAMPLES = 711
MAX_REFLECTANCE_SAMPLES = 1024
PING_INTERVAL_SECONDS = 10.0


@dataclass(frozen=True)
class Stage:
    index: int
    mission_id: int
    gps_hz: float
    reflectance_hz: float
    duration_seconds: float

    @property
    def name(self) -> str:
        return (f"gps_batch_{self.gps_hz:g}Hz_"
                f"reflectance_{self.reflectance_hz:g}Hz")


@dataclass(frozen=True)
class Offer:
    stage_index: int
    message_type: int
    sequence: int
    payload: bytes
    sent_at: float
    fragment_count: int
    wire_bytes: int
    schedule_lag_ms: float


MessageKey = tuple[int, int, int]  # mission_id, message_type, sequence


def parse_rates(value: str) -> tuple[float, ...]:
    """Parse a comma-separated rate sweep while retaining its order."""
    try:
        rates = tuple(float(item.strip()) for item in value.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "rates must be comma-separated numbers"
        ) from exc
    if not rates or any(not math.isfinite(rate) or rate < 0 for rate in rates):
        raise argparse.ArgumentTypeError("rates must be finite and non-negative")
    if any(rate > 100 for rate in rates):
        raise argparse.ArgumentTypeError("rates above 100 Hz are not supported")
    if len(set(rates)) != len(rates):
        raise argparse.ArgumentTypeError("rates must not contain duplicates")
    return rates


def percentile(values: list[float], percentage: float) -> float | None:
    """Return a nearest-rank percentile, or None for an empty sample."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(percentage / 100.0 * len(ordered)))
    return ordered[min(rank, len(ordered)) - 1]


def synthetic_payload(
    size: int, mission_id: int, message_type: int, sequence: int,
) -> bytes:
    """Build deterministic, identity-bearing bytes of an exact length."""
    if size < 13:
        raise ValueError("synthetic payload must be at least 13 bytes")
    identity = struct.pack("<QBI", mission_id, message_type, sequence)
    tail = bytes(
        ((index * 37) + sequence + message_type * 17) & 0xFF
        for index in range(size - len(identity))
    )
    return identity + tail


def reflectance_payload_bytes(sample_count: int) -> int:
    """Return the v01 serialized reflectance size for a sample count."""
    if not 1 <= sample_count <= MAX_REFLECTANCE_SAMPLES:
        raise ValueError(
            f"sample_count must be in 1..{MAX_REFLECTANCE_SAMPLES}"
        )
    return 60 + 36 + sample_count * 3 + 4


def disconnect(connection: socket.socket | None) -> None:
    if connection is None:
        return
    try:
        send_packet(connection, 0xE0, b"")
    except OSError:
        pass
    connection.close()


def _message_label(message_type: int) -> str:
    return "gps_batch" if message_type == MESSAGE_GPS_BATCH else "reflectance"


def _build_report(
    stages: list[Stage], offers: dict[MessageKey, Offer],
    completed_at: dict[MessageKey, float], mqtt_messages: int,
    mqtt_bytes: int, decoded_fragments: int, duplicate_fragments: int,
    elapsed_seconds: float, minimum_delivery: float,
) -> tuple[dict[str, object], bool]:
    stage_reports: list[dict[str, object]] = []
    all_passed = True
    for stage in stages:
        type_reports: dict[str, object] = {}
        stage_passed = True
        for message_type in (MESSAGE_GPS_BATCH, MESSAGE_REFLECTANCE):
            selected = {
                key: offer for key, offer in offers.items()
                if offer.stage_index == stage.index and
                offer.message_type == message_type
            }
            received = [key for key in selected if key in completed_at]
            latencies = [
                (completed_at[key] - selected[key].sent_at) * 1000.0
                for key in received
            ]
            offered_count = len(selected)
            received_count = len(received)
            delivery = (100.0 * received_count / offered_count
                        if offered_count else 100.0)
            kind_passed = not offered_count or delivery >= minimum_delivery
            stage_passed = stage_passed and kind_passed
            type_reports[_message_label(message_type)] = {
                "offered": offered_count,
                "received": received_count,
                "missing": offered_count - received_count,
                "delivery_percent": delivery,
                "target_hz": (stage.gps_hz if message_type == MESSAGE_GPS_BATCH
                              else stage.reflectance_hz),
                "offered_hz": offered_count / stage.duration_seconds,
                "fragments_offered": sum(
                    offer.fragment_count for offer in selected.values()
                ),
                "wire_bytes_offered": sum(
                    offer.wire_bytes for offer in selected.values()
                ),
                "latency_ms_p50": percentile(latencies, 50),
                "latency_ms_p95": percentile(latencies, 95),
                "latency_ms_max": max(latencies) if latencies else None,
                "schedule_lag_ms_p95": percentile(
                    [offer.schedule_lag_ms for offer in selected.values()], 95
                ),
                "passed": kind_passed,
            }
        all_passed = all_passed and stage_passed
        stage_reports.append({
            "name": stage.name,
            "mission_id": stage.mission_id,
            "duration_seconds": stage.duration_seconds,
            "passed": stage_passed,
            "messages": type_reports,
        })
    return ({
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "elapsed_seconds": elapsed_seconds,
        "minimum_delivery_percent": minimum_delivery,
        "passed": all_passed,
        "mqtt": {
            "payloads_received": mqtt_messages,
            "bytes_received": mqtt_bytes,
            "dtf2_fragments_decoded": decoded_fragments,
            "duplicate_dtf2_fragments": duplicate_fragments,
        },
        "stages": stage_reports,
    }, all_passed)


def _print_report(report: dict[str, object]) -> None:
    for stage in report["stages"]:  # type: ignore[index]
        outcome = "PASS" if stage["passed"] else "FAIL"
        print(
            f"STAGE {outcome} {stage['name']} "
            f"mission=0x{stage['mission_id']:016X}"
        )
        for label, item in stage["messages"].items():
            if not item["offered"]:
                continue
            p95 = item["latency_ms_p95"]
            p95_text = "n/a" if p95 is None else f"{p95:.1f}ms"
            print(
                f"  {label}: offered={item['offered']} "
                f"received={item['received']} missing={item['missing']} "
                f"delivery={item['delivery_percent']:.2f}% "
                f"latency_p95={p95_text}"
            )
    mqtt = report["mqtt"]
    print(
        f"MQTT payloads={mqtt['payloads_received']} bytes={mqtt['bytes_received']} "
        f"dtf2_fragments={mqtt['dtf2_fragments_decoded']} "
        f"duplicate_fragments={mqtt['duplicate_dtf2_fragments']}"
    )
    print("INPUT RATE TEST " + ("PASS" if report["passed"] else "FAIL"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Sweep GPS/reflectance input rates through the transparent DTU "
            "bridge and measure delivery at the MQTT broker"
        )
    )
    parser.add_argument("--serial-port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200,
                        help="PC-side USB bridge baud (default: 115200)")
    parser.add_argument("--host", required=True)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--username", default="")
    parser.add_argument("--topic", default="dji-h1/test/up")
    parser.add_argument("--expect-qos", type=int, choices=(0, 1), default=1)
    parser.add_argument(
        "--gps-batch-hz", "--gps-hz", dest="gps_hz", type=float, default=0.5,
        help=("DGB1 batch message rate; production 5 Hz source GPS becomes "
              "0.5 Hz ten-record batches (legacy alias: --gps-hz)"),
    )
    parser.add_argument("--reflectance-samples", type=int,
                        default=DEFAULT_REFLECTANCE_SAMPLES)
    parser.add_argument("--reflectance-rates", type=parse_rates,
                        default=parse_rates("0,1,2,5"))
    parser.add_argument("--stage-seconds", type=float, default=10.0)
    parser.add_argument("--drain-seconds", type=float, default=30.0)
    parser.add_argument("--fragment-gap-ms", type=float, default=0.0)
    parser.add_argument("--min-delivery-percent", type=float, default=99.0)
    parser.add_argument("--source-id", type=lambda value: int(value, 0),
                        default=0x5445535452415445)
    parser.add_argument("--report", type=Path,
                        help="optional JSON report path (must not exist)")
    args = parser.parse_args()

    for name in ("gps_hz", "stage_seconds", "drain_seconds",
                 "fragment_gap_ms", "min_delivery_percent"):
        value = getattr(args, name)
        if not math.isfinite(value):
            parser.error(f"--{name.replace('_', '-')} must be finite")
    if args.gps_hz < 0 or args.gps_hz > 100:
        parser.error("--gps-batch-hz must be in 0..100")
    if not 1 <= args.reflectance_samples <= MAX_REFLECTANCE_SAMPLES:
        parser.error(
            f"--reflectance-samples must be in 1..{MAX_REFLECTANCE_SAMPLES}"
        )
    if args.stage_seconds <= 0:
        parser.error("--stage-seconds must be positive")
    if args.drain_seconds < 0 or args.fragment_gap_ms < 0:
        parser.error("drain time and fragment gap must not be negative")
    if not 0 <= args.min_delivery_percent <= 100:
        parser.error("--min-delivery-percent must be in 0..100")
    if not 1 <= args.source_id <= 0xFFFFFFFFFFFFFFFF:
        parser.error("--source-id must be a nonzero uint64")
    if args.report is not None and args.report.exists():
        parser.error("--report path already exists")

    try:
        import serial
    except ImportError as exc:
        raise SystemExit(
            "pyserial is required for the hardware probe; install it with "
            "'python -m pip install pyserial'"
        ) from exc

    reflectance_bytes = reflectance_payload_bytes(args.reflectance_samples)

    run_seed = int(time.time() * 1000)
    base_mission = ((run_seed & 0xFFFFFFFFFFFF) << 8) | 1
    stages = [
        Stage(index, base_mission + index, args.gps_hz, reflectance_hz,
              args.stage_seconds)
        for index, reflectance_hz in enumerate(args.reflectance_rates)
    ]
    offers: dict[MessageKey, Offer] = {}
    completed_at: dict[MessageKey, float] = {}
    receiver_errors: list[BaseException] = []
    state_lock = threading.Lock()
    stop_receiver = threading.Event()
    mqtt_messages = 0
    mqtt_bytes = 0
    decoded_fragments = 0
    duplicate_fragments = 0
    seen_fragments: set[tuple[int, int, int, int]] = set()
    known_missions = {stage.mission_id for stage in stages}
    subscriber: socket.socket | None = None
    started_at = time.monotonic()

    try:
        suffix = str(run_seed)[-10:]
        subscriber = connect_client(
            args.host, args.mqtt_port,
            f"DJI_H1_rate_probe_{suffix}", args.username,
        )
        subscribe(subscriber, args.topic, 1)
        # A successful round trip proves that the SUBACK was not merely stale
        # local state and establishes readiness before any serial bytes flow.
        ping(subscriber)
        subscriber.settimeout(1.0)
        print(
            f"RATE TEST READY host={args.host}:{args.mqtt_port} "
            f"topic={args.topic} expected_qos={args.expect_qos}"
        )

        decoder = TelemetryFragmentStreamDecoder()
        estimated_messages = int(sum(
            math.ceil((stage.gps_hz + stage.reflectance_hz) *
                      stage.duration_seconds)
            for stage in stages
        ))
        reassembler = TelemetryReassembler(
            timeout_seconds=sum(stage.duration_seconds for stage in stages) +
            args.drain_seconds + 10,
            max_inflight=max(64, estimated_messages + 16),
            max_completed=max(1024, estimated_messages + 16),
            dedup_seconds=sum(stage.duration_seconds for stage in stages) +
            args.drain_seconds + 10,
        )

        def receive_worker() -> None:
            nonlocal mqtt_messages, mqtt_bytes, decoded_fragments
            nonlocal duplicate_fragments
            last_ping = time.monotonic()
            try:
                while not stop_receiver.is_set():
                    now = time.monotonic()
                    if now - last_ping >= PING_INTERVAL_SECONDS:
                        send_packet(subscriber, 0xC0, b"")
                        last_ping = now
                    try:
                        topic, mqtt_payload, qos = receive_publish(subscriber)
                    except socket.timeout:
                        continue
                    if topic != args.topic:
                        continue
                    if qos != args.expect_qos:
                        raise RuntimeError(
                            f"expected MQTT QoS {args.expect_qos}, got {qos}"
                        )
                    mqtt_messages += 1
                    mqtt_bytes += len(mqtt_payload)
                    for fragment in decoder.feed(mqtt_payload):
                        if (fragment.source_id != args.source_id or
                                fragment.mission_id not in known_missions):
                            continue
                        decoded_fragments += 1
                        fragment_key = (
                            fragment.mission_id, fragment.message_type,
                            fragment.message_sequence, fragment.fragment_index,
                        )
                        if fragment_key in seen_fragments:
                            duplicate_fragments += 1
                        else:
                            seen_fragments.add(fragment_key)
                        result = reassembler.push(fragment)
                        if result is None:
                            continue
                        key = (result.mission_id, result.message_type,
                               result.message_sequence)
                        with state_lock:
                            offer = offers.get(key)
                            if offer is None:
                                raise RuntimeError(
                                    "received an unexpected probe message"
                                )
                            if result.payload != offer.payload:
                                raise RuntimeError(
                                    "reassembled probe payload differs from source"
                                )
                            completed_at.setdefault(key, time.monotonic())
            except BaseException as exc:  # Propagate thread faults to main.
                receiver_errors.append(exc)
                stop_receiver.set()

        receiver = threading.Thread(
            target=receive_worker, name="dtu-rate-receiver", daemon=True,
        )
        receiver.start()

        sequence = {MESSAGE_GPS_BATCH: run_seed & 0xFFFFFFFF,
                    MESSAGE_REFLECTANCE: (run_seed + 0x40000000) & 0xFFFFFFFF}
        with serial.Serial(args.serial_port, args.baud, timeout=0.1) as port:
            port.dtr = False
            port.rts = False
            time.sleep(0.25)
            port.reset_input_buffer()
            for stage in stages:
                if receiver_errors:
                    raise receiver_errors[0]
                print(
                    f"STAGE START {stage.name} duration={stage.duration_seconds:g}s "
                    f"mission=0x{stage.mission_id:016X}"
                )
                stage_start = time.monotonic()
                events: list[tuple[float, int]] = []
                for message_type, rate in (
                    (MESSAGE_GPS_BATCH, stage.gps_hz),
                    (MESSAGE_REFLECTANCE, stage.reflectance_hz),
                ):
                    if rate <= 0:
                        continue
                    count = math.ceil(stage.duration_seconds * rate)
                    events.extend(
                        (index / rate, message_type) for index in range(count)
                        if index / rate < stage.duration_seconds
                    )
                # At identical deadlines GPS goes first, matching the priority
                # production gives to continuous track provenance.
                events.sort(key=lambda item: (item[0], item[1]))
                for offset, message_type in events:
                    if receiver_errors:
                        raise receiver_errors[0]
                    deadline = stage_start + offset
                    remaining = deadline - time.monotonic()
                    if remaining > 0:
                        time.sleep(remaining)
                    send_start = time.monotonic()
                    message_sequence = sequence[message_type]
                    sequence[message_type] = (message_sequence + 1) & 0xFFFFFFFF
                    size = (GPS_PAYLOAD_BYTES if message_type == MESSAGE_GPS_BATCH
                            else reflectance_bytes)
                    payload = synthetic_payload(
                        size, stage.mission_id, message_type, message_sequence,
                    )
                    fragments = fragment_message(
                        message_type, stage.mission_id, message_sequence,
                        payload, source_id=args.source_id,
                    )
                    key = (stage.mission_id, message_type, message_sequence)
                    offer = Offer(
                        stage.index, message_type, message_sequence, payload,
                        send_start, len(fragments), sum(map(len, fragments)),
                        max(0.0, (send_start - deadline) * 1000.0),
                    )
                    with state_lock:
                        offers[key] = offer
                    for fragment in fragments:
                        if port.write(fragment) != len(fragment):
                            raise IOError("short serial write")
                        port.flush()
                        if args.fragment_gap_ms:
                            time.sleep(args.fragment_gap_ms / 1000.0)
                stage_elapsed = time.monotonic() - stage_start
                print(
                    f"STAGE OFFERED {stage.name} elapsed={stage_elapsed:.3f}s"
                )

        print(f"DRAIN START duration={args.drain_seconds:g}s")
        drain_deadline = time.monotonic() + args.drain_seconds
        while time.monotonic() < drain_deadline and not receiver_errors:
            time.sleep(min(0.1, max(0.0, drain_deadline - time.monotonic())))
        if receiver_errors:
            raise receiver_errors[0]
        stop_receiver.set()
        receiver.join(3.0)
        if receiver.is_alive():
            raise TimeoutError("MQTT receiver did not stop")

        with state_lock:
            report, passed = _build_report(
                stages, dict(offers), dict(completed_at), mqtt_messages,
                mqtt_bytes, decoded_fragments, duplicate_fragments,
                time.monotonic() - started_at, args.min_delivery_percent,
            )
        report["configuration"] = {
            "serial_port": args.serial_port,
            "bridge_baud": args.baud,
            "host": args.host,
            "mqtt_port": args.mqtt_port,
            "topic": args.topic,
            "expected_qos": args.expect_qos,
            "source_id": args.source_id,
            "gps_batch_payload_bytes": GPS_PAYLOAD_BYTES,
            "reflectance_sample_count": args.reflectance_samples,
            "reflectance_payload_bytes": reflectance_bytes,
            "fragment_gap_ms": args.fragment_gap_ms,
            "drain_seconds": args.drain_seconds,
        }
        _print_report(report)
        if args.report is not None:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            with args.report.open("x", encoding="utf-8", newline="\n") as stream:
                json.dump(report, stream, indent=2)
                stream.write("\n")
            print(f"report={args.report.resolve()}")
        return 0 if passed else 2
    finally:
        stop_receiver.set()
        disconnect(subscriber)


if __name__ == "__main__":
    raise SystemExit(main())
