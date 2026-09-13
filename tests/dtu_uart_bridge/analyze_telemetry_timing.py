"""Summarize microsecond telemetry timing records from an ESP32 debug log."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import statistics


LEGACY_LINE = re.compile(
    r"TIMING type=(?P<type>\d+) seq=(?P<seq>\d+) attempt=(?P<attempt>\d+) "
    r"result=(?P<result>\S+) queue=(?P<queue>-?\d+)us "
    r"fragments=(?P<fragments>\d+) bytes=(?P<bytes>\d+) "
    r"tx_phase=(?P<tx_phase>-?\d+)us write=(?P<write>-?\d+)us "
    r"uart_tx=(?P<uart_tx>-?\d+)us gaps=(?P<gaps>-?\d+)us "
    r"ack_wait=(?P<ack_wait>-?\d+)us first_rx=(?P<first_rx>-?\d+)us "
    r"parse=(?P<parse>-?\d+)us tx_to_ack=(?P<tx_to_ack>-?\d+)us "
    r"total=(?P<total>-?\d+)us pending=(?P<pending>\d+)"
)

TX_LINE = re.compile(
    r"TX_TIMING type=(?P<type>\d+) seq=(?P<seq>\d+) attempt=(?P<attempt>\d+) "
    r"result=(?P<result>\S+) queue=(?P<queue>-?\d+)us "
    r"fragments=(?P<fragments>\d+) bytes=(?P<bytes>\d+) "
    r"tx_phase=(?P<tx_phase>-?\d+)us write=(?P<write>-?\d+)us "
    r"uart_tx=(?P<uart_tx>-?\d+)us gaps=(?P<gaps>-?\d+)us "
    r"total=(?P<total>-?\d+)us pool=(?P<pool>\d+) "
    r"inflight=(?P<inflight>\d+) ready=(?P<ready>\d+)"
)

ACK_LINE = re.compile(
    r"ACK_TIMING type=(?P<type>\d+) seq=(?P<seq>\d+) "
    r"attempts=(?P<attempts>\d+) rtt=(?P<rtt>\d+)us "
    r"pool=(?P<pool>\d+) inflight=(?P<inflight>\d+)"
)

FIELDS = (
    "queue", "tx_phase", "write", "uart_tx", "gaps", "ack_wait",
    "first_rx", "parse", "tx_to_ack", "total",
)

TX_FIELDS = (
    "queue", "tx_phase", "write", "uart_tx", "gaps", "total",
    "pool", "inflight", "ready",
)


def percentile(values: list[int], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    weight = position - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--type", type=int, default=3,
                        help="message type to summarize (default: reflectance)")
    args = parser.parse_args()

    records = []
    acknowledgements = []
    for line in args.log.read_text(encoding="latin-1").splitlines():
        match = TX_LINE.search(line)
        if match:
            record = {key: int(value) if key != "result" else value
                      for key, value in match.groupdict().items()}
            if record["type"] == args.type:
                records.append(record)
            continue
        match = ACK_LINE.search(line)
        if match:
            record = {key: int(value)
                      for key, value in match.groupdict().items()}
            if record["type"] == args.type:
                acknowledgements.append(record)
            continue
        match = LEGACY_LINE.search(line)
        if match:
            record = {key: int(value) if key != "result" else value
                      for key, value in match.groupdict().items()}
            if record["type"] == args.type:
                records.append(record)
    if not records:
        raise SystemExit(f"no telemetry timing records of type {args.type}")

    successful = [record for record in records if record["result"] == "ESP_OK"]
    asynchronous = bool(acknowledgements) or "pool" in records[0]
    retry_threshold = 1 if asynchronous else 0
    print(f"records={len(records)} successful={len(successful)} "
          f"failed={len(records) - len(successful)} "
          f"retries={sum(record['attempt'] > retry_threshold for record in records)} "
          f"acknowledged={len(acknowledgements)}")
    fields = TX_FIELDS if asynchronous else FIELDS
    for field in fields:
        values = [record[field] for record in successful if record[field] >= 0]
        if not values:
            continue
        print(
            f"{field:>10}: min={min(values):8d} mean={statistics.fmean(values):9.1f} "
            f"median={statistics.median(values):8.1f} "
            f"p95={percentile(values, 0.95):9.1f} max={max(values):8d} us"
        )
    if acknowledgements:
        rtts = [record["rtt"] for record in acknowledgements]
        print(
            f"{'ack_rtt':>10}: min={min(rtts):8d} "
            f"mean={statistics.fmean(rtts):9.1f} "
            f"median={statistics.median(rtts):8.1f} "
            f"p95={percentile(rtts, 0.95):9.1f} max={max(rtts):8d} us"
        )
        print(f"max_pool={max(record['pool'] for record in acknowledgements)} "
              f"max_inflight={max(record['inflight'] for record in acknowledgements)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
