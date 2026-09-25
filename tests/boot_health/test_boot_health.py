"""Host regression checks; optional zig C compiler runs the actual firmware checker."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
from generate_dtu_boot_profile import render  # noqa: E402


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.config = json.loads((ROOT / "tests/dtu_uart_bridge/dtu_mqtt_config.example.json").read_text())

    def test_credentials_never_embedded(self):
        self.config["mqtt"].update(client_id="private_client", username="private_user", password="private_secret")
        header = render(self.config)
        for secret in ("private_client", "private_user", "private_secret", "MQAUTH", "REBOOT"):
            self.assertNotIn(secret, header)
        self.assertIn('"AT+MQPUB1"', header)
        self.assertIn('"+UART1:460800,8,1,NONE,485"', header)
        self.assertNotIn('"AT+SOCKEN1A=', header)
        self.assertIn('"+SOCKEN1A:ON"', header)

    def test_reject_nonproduction_transport(self):
        for section, key, value in (("transport", "offline_cache", True),
                                    ("transport", "uplink_conversion", "HEX"),
                                    ("mqtt", "enabled", False),
                                    ("mcu_uart", "controller", 2)):
            config = copy.deepcopy(self.config)
            config[section][key] = value
            with self.assertRaises(ValueError):
                render(config)

    def test_escape_c_string(self):
        self.config["mqtt"]["publish"]["topic"] = 'quoted"topic'
        self.assertIn('quoted\\"topic', render(self.config))


class NativeBootTests(unittest.TestCase):
    def test_actual_c_health_and_uart_fault_paths(self):
        zig = shutil.which("zig")
        compiler = [zig, "cc"] if zig else [sys.executable, "-m", "ziglang", "cc"]
        if not zig and importlib.util.find_spec("ziglang") is None:
            self.skipTest("Install ziglang or put zig on PATH for native boot-check tests")
        with tempfile.TemporaryDirectory(prefix="dji_boot_tests_") as directory:
            executable = Path(directory) / "boot_check.exe"
            command = compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(HERE / "stubs"),
                "-I" + str(ROOT / "components/boot_health/include"),
                "-I" + str(ROOT / "components/telemetry"),
                str(HERE / "native_check.c"),
                str(ROOT / "components/boot_health/boot_health.c"),
                str(ROOT / "components/telemetry/dtu_boot_check.c"),
                "-o", str(executable)]
            # Zig's first Windows C-runtime build can take several minutes.
            built = subprocess.run(command, capture_output=True, text=True, timeout=300)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for scenario in range(12):
                with self.subTest(scenario=scenario):
                    result = subprocess.run([str(executable), str(scenario)], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
