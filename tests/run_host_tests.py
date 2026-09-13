#!/usr/bin/env python3
"""Run every repository host-only unittest group and reject empty discovery."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent


def main() -> int:
    directories = sorted({path.parent for path in TEST_ROOT.glob("*/test_*.py")})
    if not directories:
        print("ERROR: no host-test directories were found", file=sys.stderr)
        return 2

    combined = unittest.TestSuite()
    total = 0
    for directory in directories:
        suite = unittest.TestLoader().discover(
            str(directory), pattern="test_*.py")
        count = suite.countTestCases()
        print(f"Discovered {count:>3} test(s) in {directory.name}", flush=True)
        if count == 0:
            print(f"ERROR: empty discovery in {directory}", file=sys.stderr)
            return 2
        combined.addTests(suite)
        total += count

    print(f"Running {total} host test(s) from {len(directories)} groups\n",
          flush=True)
    result = unittest.TextTestRunner(verbosity=2).run(combined)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
