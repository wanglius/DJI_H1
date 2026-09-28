"""Compile the production loss tracker and SC16 tick contract on the host."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


class AcquisitionHealthTests(unittest.TestCase):
    def setUp(self):
        zig = shutil.which("zig")
        if not zig and importlib.util.find_spec("ziglang") is None:
            self.skipTest("Install ziglang or put zig on PATH for native tests")
        self.compiler = [zig, "cc"] if zig else [sys.executable, "-m", "ziglang", "cc"]

    def compile(self, source, output, *flags):
        return subprocess.run(self.compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "main"),
            "-I" + str(ROOT / "components/sc16is752/include"), *flags,
            str(HERE / source), "-o", str(output)],
            capture_output=True, text=True, timeout=300)

    def test_loss_tracking_freeze_restart_and_wrap(self):
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "rx_loss.exe"
            result = self.compile("native_rx_loss.c", executable)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(executable)], check=True, timeout=5)

    def test_tick_rate_contract(self):
        with tempfile.TemporaryDirectory() as folder:
            for hz in (1000, 100, 500):
                with self.subTest(hz=hz):
                    result = self.compile("tick_contract.c", Path(folder) / "tick.exe",
                                          f"-DconfigTICK_RATE_HZ={hz}")
                    if hz == 1000:
                        self.assertEqual(result.returncode, 0, result.stderr)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("SC16 dual-RX polling requires", result.stderr)


if __name__ == "__main__":
    unittest.main()
