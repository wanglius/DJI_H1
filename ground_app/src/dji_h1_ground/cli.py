"""CLI for live GUI, SD viewer, headless receiver, replay and JSONL export."""
from __future__ import annotations

import argparse
import json
import sys

from .config import GroundConfig
from .receiver import GroundReceiver
from .api import start_http_api
from .journal import export_jsonl


def main():
    parser = argparse.ArgumentParser(description="DJI H1 ground station / 地面接收站")
    parser.add_argument("--config", default="config/ground_app.local.json")
    parser.add_argument("--check-config", action="store_true")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--live", action="store_true", help="GUI + MQTT")
    modes.add_argument("--headless", action="store_true", help="MQTT + JSONL stdout + HTTP; no Qt")
    modes.add_argument("--mission", help="view an SD mission folder")
    modes.add_argument("--replay", help="replay SQLite journal to JSONL stdout")
    modes.add_argument("--export", help="export decoded SQLite messages to --output")
    parser.add_argument("--output", help="new JSONL file for --export")
    parser.add_argument("--speed", type=float, default=0, help="replay speed, 0=fastest, 1=real time")
    args = parser.parse_args()
    if args.export:
        if not args.output:
            parser.error("--export requires --output")
        export_jsonl(args.export, args.output)
        return
    try:
        config = GroundConfig.load(args.config)
    except ValueError as exc:
        parser.error(str(exc))
    if args.check_config:
        print("Configuration valid; secrets omitted.")
        print(config.map.error or ("Map enabled" if config.map.enabled else "Offline map"))
        return
    if args.replay:
        receiver = GroundReceiver(config)
        for message in receiver.replay(args.replay, speed=args.speed):
            print(json.dumps(message.to_dict(), ensure_ascii=False), flush=True)
        return
    if args.headless:
        server = None
        with GroundReceiver(config) as receiver:
            try:
                if config.api_enabled:
                    server, _ = start_http_api(receiver.service, port=config.api_port)
                    print(f"API: http://127.0.0.1:{server.server_port}/api/v1", file=sys.stderr)
                for message in receiver.iter_messages():
                    print(json.dumps(message.to_dict(), ensure_ascii=False), flush=True)
            except KeyboardInterrupt:
                pass
            finally:
                if server:
                    server.shutdown()
                    server.server_close()
        return
    try:
        from .ui import run_desktop
    except ImportError:
        parser.error("GUI requires: python -m pip install -e '.[gui]' (map optional: .[map])")
    run_desktop(initial_path=args.mission,
                initial_live_config=args.config if args.live else None,
                api_port=config.api_port if config.api_enabled else None,
                ground_config=config)
