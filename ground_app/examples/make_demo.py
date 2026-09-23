"""生成独立 SD 示例目录：python examples/make_demo.py data/demo_mission

不访问 MQTT，不使用真实坐标记录；输出目录必须不存在。
"""
import json
from pathlib import Path
import struct
import sys
import zlib


def record(kind, seq, millis, body):
    header = struct.pack("<IHHIIIHHIQQQIHBB", 0x31524844, 1, kind, 60,
                         64 + len(body), 1, 1, 0, seq,
                         millis * 1000, millis, 1789430400000 + millis,
                         1, 1, 1, 7)
    encoded = header + body
    return encoded + struct.pack("<I", zlib.crc32(encoded) & 0xFFFFFFFF)


def main():
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=False)
    gps, spectra = [], []
    for i in range(61):
        millis = 1000 + i * 1000
        body = struct.pack("<B3xiiiIIH8B", i, 399000000 + i * 50,
                           1164000000 + i * 100, 50000, 1789430400 + millis // 1000,
                           millis, 0, 3, 4, 2, 1, 6, 88, 0, 15)
        gps.append(record(1, i + 1, millis, body))
        if i < 60:
            # Match the production H1 grid: 340..1050 nm, inclusive.
            samples = [1000 + j * 10 + i for j in range(711)]
            prefix = struct.pack("<IIIQIHHHHHH", i + 1, i + 1, i + 1,
                                 millis * 1000, 25000, 711, 711, 0, 0, 0, 15)
            spectra.append(record(3, i + 1, millis + 500,
                                  prefix + struct.pack("<711H", *samples) + bytes([1] * 711)))
    for filename, kind, records in (("GPS_TRACK.BIN", 1, gps), ("REFLECTANCE.BIN", 3, spectra)):
        (root / filename).write_bytes(struct.pack("<IHHII", 0x31464844, 1, kind, 16, 0)
                                     + b"".join(records))
    (root / "MISSION.JSON").write_text(json.dumps({
        "schema_version": 1, "directory": "SYNTHETIC_DEMO", "state": "closed",
        "drone_serial": "DEMO_ONLY", "drone_serial_hex": "44454D4F",
        "timezone": {"name": "Asia/Shanghai", "utc_offset_minutes": 480}
    }), encoding="utf-8")
    (root / "EVENTS.JSONL").write_text(json.dumps({
        "sequence": 1, "event": "ab_link_lost", "severity": "critical",
        "b_monotonic_us": 30500000, "utc_ms": 1789430430500
    }) + "\n", encoding="utf-8")
    print(root.resolve())


if __name__ == "__main__":
    main()
