"""Compile and execute production C with fake UART/time/queues, without hardware.

CC may name a host C compiler. The optional ziglang package supplies a portable
host compiler on Windows without installing a Windows SDK. No firmware
headers/implementations are copied: only platform services are replaced.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def compiler():
    configured = os.environ.get('CC')
    if configured:
        return [configured]
    try:
        import ziglang
        executable = Path(ziglang.__file__).parent/('zig.exe' if sys.platform == 'win32' else 'zig')
        return [str(executable), 'cc']
    except ImportError:
        host = shutil.which('cc') or shutil.which('clang')
        return [host] if host else None


def build_executable(cc, source, output):
    includes = [HERE/'stubs'] + sorted((ROOT/'components').glob('*/include'))
    flags = [item for path in includes for item in ('-I', str(path))]
    env = dict(os.environ)
    env['ZIG_GLOBAL_CACHE_DIR'] = str(ROOT/'build-native-tests'/'zig-cache')
    subprocess.run([*cc, '-O1', '-ffunction-sections',
                    '-fdata-sections', '-fvisibility=hidden', '-Wl,--gc-sections', *flags,
                    str(source), '-o', str(output)], check=True, capture_output=True, env=env, timeout=300)


class NativeFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = compiler()
        if not cc:
            raise unittest.SkipTest('Host C compiler unavailable; set CC to enable native firmware tests')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        for name in ('modem', 'pool'):
            output = Path(cls.temp.name)/(name+('.exe' if sys.platform == 'win32' else ''))
            try:
                build_executable(cc, HERE/(name+'_harness.c'), output)
            except subprocess.CalledProcessError as exc:
                raise RuntimeError((exc.stdout+exc.stderr).decode(errors='replace')) from exc
            setattr(cls, name, output)

    def run_case(self, executable, mode, case):
        result = subprocess.run([str(executable), str(mode), str(case)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_modem_fault_and_success_paths(self):
        for case in range(9):
            with self.subTest(case=case):
                self.run_case(self.modem, 0, case)

    def test_binary_ack_all_uart_chunk_sizes(self):
        for chunk in range(1, 80):
            with self.subTest(chunk=chunk):
                self.run_case(self.modem, 1, chunk)

    def test_offline_pool_and_shutdown_ownership(self):
        for case in range(4):
            with self.subTest(case=case):
                self.run_case(self.pool, 0, case)


if __name__ == '__main__':
    unittest.main()
