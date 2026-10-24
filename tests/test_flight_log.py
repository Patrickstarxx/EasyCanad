"""Host capture checks use a fake serial transport; no connected Arduino is required."""
import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("flight_log", ROOT / "tools/flight_log.py")
flight_log = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flight_log)


def metadata():
    config = struct.pack("<8f3H3bB", 1.2, 90, 90, 28333.333, 360, 1, 0.5, 500000,
                         500, 1500, 2500, 1, 1, 1, 1)
    return f"C,4,{config.hex()},{struct.pack('<f', 12.5).hex()}\n"


class FakeConnection:
    def __init__(self):
        self.lines = [b"D,old-session\n", metadata().encode("ascii"), b"R,snapshot\n", b"D,new-session\n"]
        self.commands = []

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.commands.append(data)

    def readline(self):
        return self.lines.pop(0) if self.lines else b""


class CaptureTests(unittest.TestCase):
    def test_configuration_decoder(self):
        config = flight_log.read_configuration(metadata())
        self.assertAlmostEqual(config["gain"], 1.2, places=6)
        self.assertEqual(config["zero_deg"], 12.5)
        self.assertEqual(config["throttle_schedule"], 1)
        with self.assertRaises(ValueError):
            flight_log.read_configuration("C,3,00,00")

    def test_v6_configuration_decoder(self):
        v5 = struct.pack("<8f3H3bB4fB", 1.2, 90, 30, 28333.333, 360, 1, 0.5, 500000,
                         700, 1400, 2300, 1, 1, 1, 0, 800, 1000, 20, 5, 2)
        v6 = v5 + struct.pack("<3fB", 5, 3, 0.5, 1)
        config = flight_log.read_configuration(f"C,6,{v6.hex()},{struct.pack('<f', 0).hex()}")
        self.assertEqual(config["log_version"], 6)
        self.assertEqual(config["trim_deg"], 5)
        self.assertEqual(config["aoa_reference_deg"], 3)
        self.assertEqual(config["compensation_ratio"], 0.5)
        self.assertEqual(config["law_mode"], 1)

    def test_capture_waits_for_new_metadata(self):
        connection = FakeConnection()
        serial_module = types.SimpleNamespace(Serial=lambda *args, **kwargs: connection)
        with tempfile.TemporaryDirectory(prefix="easycanad-capture-test-") as directory:
            output = Path(directory) / "logs" / "test.ecsv"
            args = types.SimpleNamespace(output=output, port="FAKE", baud=500000, duration=1)
            with patch.dict(sys.modules, serial=serial_module), patch.object(flight_log.time, "sleep"), \
                 patch.object(flight_log.time, "monotonic", side_effect=[i / 10 for i in range(100)]), \
                 contextlib.redirect_stdout(io.StringIO()):
                flight_log.capture(args)
            saved = output.read_text(encoding="ascii")
            self.assertNotIn("old-session", saved)
            self.assertIn("C,4,", saved)
            self.assertIn("D,new-session", saved)
            self.assertEqual(connection.commands, [b"log start\n"])
            before = output.read_bytes()
            with patch.dict(sys.modules, serial=serial_module), self.assertRaises(SystemExit):
                flight_log.capture(args)
            self.assertEqual(output.read_bytes(), before)

    def test_empty_capture_reports_failure(self):
        connection = FakeConnection()
        connection.lines = []
        with tempfile.TemporaryDirectory(prefix="easycanad-empty-test-") as directory:
            args = types.SimpleNamespace(output=Path(directory) / "test.ecsv", port="FAKE", baud=500000, duration=1)
            with patch.dict(sys.modules, serial=types.SimpleNamespace(Serial=lambda *a, **k: connection)), \
                 patch.object(flight_log.time, "sleep"), \
                 patch.object(flight_log.time, "monotonic", side_effect=[i / 10 for i in range(100)]), \
                 contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                flight_log.capture(args)


if __name__ == "__main__":
    unittest.main()
