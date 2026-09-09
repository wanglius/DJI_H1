"""Command-line entry point for ``python -m dji_h1_viewer``."""

from __future__ import annotations

import argparse


def main() -> None:
    parser = argparse.ArgumentParser(description="DJI H1 mission data viewer")
    parser.add_argument("mission", nargs="?", help="mission folder to open")
    parser.add_argument("--api-port", type=int, default=8765,
                        help="localhost HTTP API port (default: 8765; 0 chooses a free port)")
    parser.add_argument("--no-api", action="store_true",
                        help="do not start the localhost HTTP API")
    parser.add_argument("--skip-crc", action="store_true",
                        help="skip the initial CRC scan (faster but unsafe)")
    args = parser.parse_args()
    try:
        from .ui import run_desktop
    except ModuleNotFoundError as exc:
        if exc.name and exc.name.startswith("PyQt6"):
            parser.error(
                "PyQt6 is required for the GUI; install the package with "
                "'python -m pip install -e tools/mission_viewer'")
        raise
    run_desktop(initial_path=args.mission,
                api_port=None if args.no_api else args.api_port,
                verify_crc=not args.skip_crc)


if __name__ == "__main__":
    main()
