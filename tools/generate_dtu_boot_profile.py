"""Compile ONLY non-secret YY-M200 readback expectations from provisioning JSON.

No MQAUTH query/value is emitted. Authentication and routing are proven by a
matching application ACK, not by reading credentials back from the modem.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "dtu_uart_bridge"))
from configure_dtu_mqtt import build_settings, load_config  # noqa: E402


def render(config: dict) -> str:
    uart = config["mcu_uart"]
    mqtt = config["mqtt"]
    transport = config["transport"]
    if (uart["controller"] != 1 or not mqtt["enabled"] or
            not mqtt["publish"]["enabled"] or not mqtt["subscribe"]["enabled"] or
            transport["uplink_conversion"] != "RAW" or
            transport["downlink_conversion"] != "RAW" or
            transport["offline_cache"]):
        raise ValueError("Boot profile requires UART1, enabled MQTT pub/sub, RAW and no cache")
    lines = ["/* Generated non-secret read-only expectations; do not edit. */",
             f"#define DTU_PROFILE_BAUD {uart['baud_rate']}U",
             f"#define DTU_PROFILE_TX_GPIO {uart['tx_gpio']}",
             f"#define DTU_PROFILE_RX_GPIO {uart['rx_gpio']}",
             "static const struct { const char *query; const char *expected; }",
             "s_profile[] = {"]
    for label, command in build_settings(config):
        if label in ("MQTT authentication", "disable Socket A"):
            continue
        query, value = command.split("=", 1)
        lines.append(f"    {{{json.dumps(query)}, {json.dumps(query[2:] + ':' + value)}}},")
    lines.append("};\n")
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(render(load_config(args.config)), encoding="utf-8")
