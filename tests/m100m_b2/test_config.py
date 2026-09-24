"""Build-time secret configuration validation; no hardware or network."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from generate_m100m_config import generate


class ConfigurationTests(unittest.TestCase):
    def test_example_builds_and_unsafe_fields_fail(self):
        cfg = json.loads((ROOT/'config/m100m.example.json').read_text())
        with tempfile.TemporaryDirectory() as folder:
            source, dest = Path(folder)/'input.json', Path(folder)/'config.h'
            source.write_text(json.dumps(cfg))
            generate(source, dest)
            self.assertIn('s_m100m_config', dest.read_text())
            for key, value in [('host','https://example.com'),('port',True),
                    ('port',0),('password','bad\r\nAT'),('password','bad"'),
                    ('uplink_topic','test/#'),('ack_topic',cfg['uplink_topic'])]:
                source.write_text(json.dumps({**cfg,key:value}))
                with self.subTest(key=key):
                    with self.assertRaises(ValueError): generate(source,dest)
