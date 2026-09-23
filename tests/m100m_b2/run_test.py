"""Isolated Air780 native-AT MQTT benchmark: one DHR1 per publication.

PC simulates the producer and the ground receiver, but each DTA1 must traverse
broker -> cellular modem -> physical COM port before freeing a retained entry.
No production firmware/config is modified. No flash/reset/persistent AT&W.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import json
import math
from pathlib import Path
import queue
import re
import secrets
import struct
import sys
import threading
import time
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/mission_viewer"))
from dji_h1_viewer.decoder import decode_record
from dji_h1_viewer.telemetry import (
    ReassembledTelemetry, decode_acknowledgement, encode_acknowledgement,
)


def spectrum(sequence: int, session: int) -> bytes:
    """Valid synthetic 711-bin record; all bytes, including NUL, travel unchanged."""
    stamp = sequence * 200000
    header = struct.pack("<IHHIIIHHIQQQIHBB", 0x31524844, 1, 3, 60, 2233,
                         session, 1, 0, sequence, stamp, 0, 0, 0, 0, 0, 1)
    info = struct.pack("<IIIQIHHHHHH", sequence, sequence, sequence,
                       stamp - 10000, 10000, 711, 711, 0, 0, 0, 15)
    values = struct.pack("<711H", *((i * 37 + sequence) % 10001 for i in range(711)))
    body = header + info + values + bytes([1] * 711)
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


class AtParser:
    """Length-aware +MSUB parsing: binary ACK may contain CR/LF, > or OK."""
    prefix = re.compile(rb'^\+MSUB:\s*"([^"\r\n]*)",\s*(\d+)\s*byte,')

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, chunk):
        self.buffer.extend(chunk)
        result = []
        while self.buffer:
            if self.buffer.startswith(b"\r\n"):
                del self.buffer[:2]
                continue
            if self.buffer.startswith(b"+MSUB:"):
                match = self.prefix.match(self.buffer)
                if not match:
                    if len(self.buffer) > 512:
                        raise ValueError("invalid MSUB header")
                    break
                size = int(match[2])
                if size > 4100:
                    raise ValueError("oversize MSUB")
                end = match.end() + size
                if len(self.buffer) < end:
                    break
                result.append(("message", (match[1].decode(), bytes(self.buffer[match.end():end]))))
                del self.buffer[:end]
                continue
            if self.buffer.startswith(b">"):
                del self.buffer[:1]
                if self.buffer.startswith(b" "):
                    del self.buffer[:1]
                result.append(("line", ">"))
                continue
            end = self.buffer.find(b"\r\n")
            if end < 0:
                if len(self.buffer) > 8192:
                    raise ValueError("unbounded AT response")
                break
            line = bytes(self.buffer[:end]).decode("ascii", errors="replace").strip()
            del self.buffer[:end + 2]
            if line:
                result.append(("line", line))
        return result


class Modem:
    def __init__(self, cfg, journal):
        import serial
        self.serial = serial.Serial(port=None, baudrate=cfg["initial_baud"],
                                    timeout=.005, write_timeout=3)
        self.serial.port = cfg["serial_port"]
        self.serial.dtr = self.serial.rts = False
        self.serial.open()
        self.parser = AtParser()
        self.lines = []
        self.message_handler = lambda topic, data: None
        self.journal = journal
        self.pubacks = 0
        self.publish_busy = False

    def pump(self):
        data = self.serial.read(self.serial.in_waiting or 1)
        for kind, value in self.parser.feed(data):
            if kind == "message":
                self.message_handler(*value)
            else:
                self.journal("at_rx", line=value)
                if value == "PUBACK":
                    self.pubacks += 1
                    self.publish_busy = False
                self.lines.append(value)
                self.lines = self.lines[-100:]

    def wait(self, wanted="OK", timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while self.lines:
                line = self.lines.pop(0)
                if line == wanted:
                    return
                if line == "ERROR" or line.startswith("+CME ERROR") or "FAIL" in line:
                    raise RuntimeError(f"AT failure: {line}")
            self.pump()
        raise TimeoutError(f"AT timeout waiting for {wanted}")

    def command(self, command, wanted="OK", timeout=5, secret=False):
        self.lines.clear()
        self.journal("at_tx", command="AT+MCONFIG=<redacted>" if secret else command)
        self.write(command.encode("ascii") + b"\r\n")
        self.wait(wanted, timeout)

    def write(self, data):
        if self.serial.write(data) != len(data):
            raise IOError("short UART write")

    def publish(self, topic, data):
        # Wait only for local prompt/acceptance, NOT PUBACK or application ACK.
        self.command(f'AT+MPUBEX="{topic}",1,0,{len(data)}', ">", 3)
        self.lines.clear()
        self.write(data)
        self.publish_busy = True
        self.wait("OK", 3)


def quote(value):
    if not isinstance(value, str) or any(c in value for c in '\r\n"\\'):
        raise ValueError("unsupported AT string characters")
    value.encode("ascii")
    return '"' + value + '"'


def stats(values):
    values = sorted(values)
    return {"count": len(values), "mean": sum(values)/len(values) if values else None,
            "p95": values[math.ceil(.95*len(values))-1] if values else None,
            "max": max(values) if values else None}


@dataclass
class Entry:
    payload: bytes
    offered: float
    first_sent: float | None = None
    last_sent: float | None = None
    attempts: int = 0


def ack_matches(ack, source, mission, seq, payload):
    return (ack.source_id == source and ack.mission_id == mission and
            ack.message_type == 3 and ack.message_sequence == seq and
            ack.message_crc32 == zlib.crc32(payload) & 0xFFFFFFFF)


def run(cfg, output):
    import paho.mqtt.client as mqtt
    hz = float(cfg["hz"])
    duration = float(cfg["duration_seconds"])
    ttl = float(cfg["lifespan_seconds"])
    ack_timeout = float(cfg["ack_timeout_seconds"])
    if not all(math.isfinite(x) and x > 0 for x in (hz, duration, ttl, ack_timeout)):
        raise ValueError("rates and times must be finite and positive")
    if not 1 <= hz <= 20 or not 1 <= duration <= 600 or not 1 <= ttl <= 60:
        raise ValueError("scratch limit: 1..20 Hz, 1..600 seconds, 1..60 seconds TTL")
    count = int(hz * duration)
    capacity = int(cfg["pool_capacity"])
    if not 1 <= capacity <= 512:
        raise ValueError("pool_capacity must be 1..512")
    # Validate all config before touching the modem; keep credentials off stdout.
    for name in ("host", "username", "password", "topic_prefix"):
        quote(cfg[name])
    if any(c in cfg["topic_prefix"] for c in '#+'):
        raise ValueError("wildcards not allowed in dedicated test topics")
    run_id = secrets.token_hex(6)
    source = 0x4D3130304D4232
    mission = secrets.randbits(63) or 1
    session = mission & 0xFFFFFFFF
    up = cfg["topic_prefix"].rstrip('/') + '/' + run_id + '/up'
    down = cfg["topic_prefix"].rstrip('/') + '/' + run_id + '/down'
    expected = {seq: spectrum(seq, session) for seq in range(1, count+1)}
    output.mkdir(parents=True, exist_ok=False)
    log = (output / "events.jsonl").open("w", encoding="utf-8")
    lock = threading.Lock()

    def journal(event, **values):
        with lock:
            log.write(json.dumps({"t": time.monotonic(), "event": event, **values}) + '\n')
            log.flush()

    ingress = queue.Queue(maxsize=512)
    ready = threading.Event()
    stop = threading.Event()
    failures = queue.Queue()
    received = {}
    acked = {}
    counters = Counter()
    sizes = Counter()
    pool = {}
    schedule_lags, submit_ms, ack_rtt = [], [], []
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id='M100M_ground_' + run_id, protocol=mqtt.MQTTv311)
    if cfg["username"]:
        client.username_pw_set(cfg["username"], cfg["password"])

    def on_connect(c, userdata, flags, reason, properties):
        if reason.is_failure:
            failures.put("ground MQTT connection rejected")
        else:
            result, _ = c.subscribe(up, qos=1)
            if result:
                failures.put("ground subscribe failed")

    def on_subscribe(c, userdata, mid, reasons, properties):
        if not reasons or any(x.is_failure for x in reasons):
            failures.put("ground SUBACK rejected")
        else:
            ready.set()

    def on_message(c, userdata, msg):
        try:
            ingress.put_nowait((msg.topic, bytes(msg.payload), msg.qos, time.monotonic()))
        except queue.Full:
            failures.put("ground ingress overflow")

    def worker():
        while not stop.is_set() or not ingress.empty():
            try:
                topic, payload, qos, instant = ingress.get(timeout=.1)
            except queue.Empty:
                continue
            try:
                if topic != up:
                    raise ValueError("unexpected topic")
                record = decode_record(payload, expected_type=3)
                seq = record.header.sequence
                if payload != expected.get(seq) or qos != 1:
                    raise ValueError("payload identity/bytes or QoS mismatch")
                sizes[len(payload)] += 1
                duplicate = seq in received
                received.setdefault(seq, instant)
                journal("received", sequence=seq, bytes=len(payload), duplicate=duplicate)
                ack = encode_acknowledgement(ReassembledTelemetry(
                    3, source, mission, seq, 0, 1, payload))
                if client.publish(down, ack, qos=0, retain=False).rc:
                    raise RuntimeError("ground DTA1 publish failed")
                # Duplicates are re-ACKed, but not counted as new records.
            except Exception as exc:
                failures.put(str(exc))
            finally:
                ingress.task_done()

    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_message = on_message
    thread = None
    modem = None
    error = None
    admitted = 0
    finished = set()
    start = None
    try:
        client.connect(cfg["host"], int(cfg["port"]), keepalive=30)
        client.loop_start()
        thread = threading.Thread(target=worker, name="ground-validator", daemon=True)
        thread.start()
        if not ready.wait(15):
            raise TimeoutError("ground receiver not SUBACK-ready; no load sent")
        print(f"GROUND READY: {up}", flush=True)
        modem = Modem(cfg, journal)
        modem.command("AT")
        modem.command("ATE0")
        modem.command("ATI")
        modem.command("AT+CGATT?")
        if cfg["baud"] != cfg["initial_baud"]:
            modem.command(f'AT+IPR={int(cfg["baud"])}')
            modem.serial.baudrate = int(cfg["baud"])
            modem.command("AT")
        modem.command("AT+MQTTMODE=0")
        modem.command("AT+MQTTMSGSET=0")
        modem.command('AT+MCONFIG=' + ','.join(map(quote, (
            'M100M_device_' + run_id, cfg["username"], cfg["password"]))), secret=True)
        modem.command(f'AT+MIPSTART={quote(cfg["host"])},"{int(cfg["port"])}"',
                      "CONNECT OK", 30)
        modem.command("AT+MCONNECT=1,120", "CONNACK OK", 15)
        modem.command(f'AT+MSUB={quote(down)},0', "SUBACK", 15)

        def on_ack(topic, payload):
            if topic != down:
                counters["foreign_downlink"] += 1
                return
            try:
                ack = decode_acknowledgement(payload)
            except ValueError:
                counters["invalid_ack"] += 1
                return
            seq = ack.message_sequence
            entry = pool.get(seq)
            if entry is None or entry.first_sent is None:
                counters["late_or_duplicate_ack"] += 1
                return
            if not ack_matches(ack, source, mission, seq, entry.payload):
                counters["mismatched_ack"] += 1
                return
            if ack.status != 0:
                raise RuntimeError("ground rejected record")
            instant = time.monotonic()
            acked[seq] = instant
            ack_rtt.append((instant-entry.first_sent)*1000)
            journal("ack_on_uart", sequence=seq, rtt_ms=ack_rtt[-1])
            del pool[seq]
            finished.add(seq)

        modem.message_handler = on_ack
        start = time.monotonic()
        next_progress = start
        while time.monotonic() < start + duration + ttl + 3:
            modem.pump()
            now = time.monotonic()
            if not failures.empty():
                raise RuntimeError(failures.get())
            while admitted < count and now >= start + admitted/hz:
                seq = admitted + 1
                if len(pool) >= capacity:
                    counters["pool_overflow"] += 1
                    finished.add(seq)
                else:
                    pool[seq] = Entry(expected[seq], start + admitted/hz)
                admitted += 1
            for seq, entry in list(pool.items()):
                if now-entry.offered >= ttl:
                    counters["expired_sent" if entry.attempts else "expired_unsent"] += 1
                    journal("expired", sequence=seq, attempts=entry.attempts)
                    del pool[seq]
                    finished.add(seq)
            counters["pool_peak"] = max(counters["pool_peak"], len(pool))
            counters["unsent_peak"] = max(counters["unsent_peak"], sum(e.attempts == 0 for e in pool.values()))
            counters["awaiting_ack_peak"] = max(counters["awaiting_ack_peak"], sum(e.attempts > 0 for e in pool.values()))
            candidates = [(seq, e) for seq, e in pool.items() if e.attempts == 0]
            if not candidates:
                candidates = [(seq, e) for seq, e in pool.items()
                              if e.attempts == 1 and now-e.last_sent >= ack_timeout]
            # Native AT firmware allows one QoS-1 publish awaiting PUBACK.
            # Respect that modem flow control while still accepting samples,
            # processing ground DTA1s, expiring entries and measuring backlog.
            if candidates and not modem.publish_busy:
                seq, entry = candidates[0]
                began = time.monotonic()
                if entry.attempts == 0:
                    entry.first_sent = began
                    schedule_lags.append((began-entry.offered)*1000)
                    counters["sent_once"] += 1
                else:
                    counters["retries"] += 1
                entry.last_sent = began
                entry.attempts += 1
                modem.publish(up, entry.payload)
                submit_ms.append((time.monotonic()-began)*1000)
                journal("submitted", sequence=seq, attempts=entry.attempts, duration_ms=submit_ms[-1])
            if now >= next_progress:
                print(f"offered={admitted}/{count} rx={len(received)} ack={len(acked)} pool={len(pool)}", flush=True)
                next_progress = now + 10
            if admitted == count and len(finished) == count:
                break
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
        print(error, flush=True)
    finally:
        if modem:
            # A failed publish may still be in data-entry mode: don't blindly
            # send cleanup commands then. Close COM; caller can recover explicitly.
            if error is None:
                try:
                    modem.command("AT+MDISCONNECT")
                    modem.command("AT+MIPCLOSE")
                except Exception as exc:
                    journal("cleanup_error", error=str(exc))
            modem.serial.close()
        client.disconnect()
        client.loop_stop()
        stop.set()
        if thread:
            thread.join(5)
        if thread and thread.is_alive():
            error = error or "ground worker did not stop"
        if not failures.empty():
            error = error or failures.get()
        counters["pubacks"] = modem.pubacks if modem else 0
        passed = (error is None and len(received) == count and len(acked) == count
                  and counters["sent_once"] == count and not pool
                  and not any(counters[k] for k in ("pool_overflow", "expired_sent", "expired_unsent", "invalid_ack", "mismatched_ack"))
                  and max(schedule_lags, default=1e9) < 1000/hz)
        report = {"passed": passed, "error": error, "requested_hz": hz,
                  "duration_seconds": duration, "record_bytes": 2233,
                  "application_fragmentation": False, "uplink_qos": 1, "ack_qos": 0,
                  "modem_flow_control": "one publish until PUBACK (not ground ACK)",
                  "source_id_hex": f"{source:016X}", "mission_id_hex": f"{mission:016X}",
                  "uplink_topic": up, "ack_topic": down, "uart_baud": cfg["baud"],
                  "expected": count, "offered": admitted, "received_unique": len(received),
                  "acknowledged_on_uart": len(acked), "remaining_pool": len(pool),
                  "missing_received": sorted(set(expected)-received.keys()),
                  "missing_ack": sorted(set(expected)-acked.keys()),
                  "mqtt_payload_sizes": dict(sizes), "counters": dict(counters),
                  "ack_rtt_ms": stats(ack_rtt), "at_submit_ms": stats(submit_ms),
                  "schedule_lag_ms": stats(schedule_lags)}
        (output / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        log.close()
    print(json.dumps({k:v for k,v in report.items() if not k.startswith('missing_')}, indent=2))
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration", type=float)
    args = parser.parse_args()
    cfg = json.loads(args.config.read_text(encoding="utf-8-sig"))
    if args.duration is not None:
        cfg["duration_seconds"] = args.duration
    return run(cfg, args.output)


if __name__ == '__main__':
    raise SystemExit(main())
