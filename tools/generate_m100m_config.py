"""Build-only generator. Never print credentials; generated header stays in build/."""
import json
from pathlib import Path
import sys


def generate(source, destination):
    cfg = json.loads(Path(source).read_text(encoding="utf-8-sig"))
    fields = ("host", "username", "password", "uplink_topic", "ack_topic")
    for field in fields:
        value = cfg[field]
        if not isinstance(value, str) or len(value) > 180 or any(
                ord(c) < 32 or ord(c) > 126 or c in '\\"' for c in value):
            raise ValueError(f"Invalid AT configuration field: {field}")
    for field in ("host", "uplink_topic", "ack_topic"):
        if not cfg[field] or any(c in cfg[field] for c in '+#'):
            raise ValueError(f"Empty or wildcard field: {field}")
    if cfg["uplink_topic"] == cfg["ack_topic"]:
        raise ValueError("Uplink and ACK topics must differ")
    if any(c in cfg['host'] for c in ' /:'):
        raise ValueError("Host must be a DNS name or IPv4 address, not a URL")
    if type(cfg["port"]) is not int or not 1 <= cfg["port"] <= 65535:
        raise ValueError("Invalid broker port")
    text = '#pragma once\n#include "m100m.h"\nstatic const m100m_config_t s_m100m_config = {\n'
    text += ''.join(f'    .{key} = {json.dumps(cfg[key])},\n' for key in fields)
    text += f'    .port = {cfg["port"]},\n}};\n'
    Path(destination).write_text(text, encoding="ascii")


if __name__ == "__main__":
    generate(*sys.argv[1:])
