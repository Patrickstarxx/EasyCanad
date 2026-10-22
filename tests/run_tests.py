"""Compile the real sketch against hardware doubles, then run isolated scenarios."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "startup", "legacy", "record", "power-loss", "rc-capture", "rc-rollover",
    "rc-loss-output", "mapping-mixing", "sensor", "manual-fallback",
    "direction-rate", "calibration-seam", "calibration-reject", "button", "telemetry",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    compiler = [args.compiler]
    if Path(args.compiler).stem == "zig":
        compiler.append("c++")
    source = (ROOT / "EasyCanad.ino").read_text(encoding="utf-8")
    variants = {
        "default": source,
        "limited-asymmetric": source.replace("canardLimitDeg = 90.0f", "canardLimitDeg = 30.0f")
            .replace("SERVO_MIN_US = 500", "SERVO_MIN_US = 700")
            .replace("SERVO_MID_US = 1500", "SERVO_MID_US = 1400")
            .replace("SERVO_MAX_US = 2500", "SERVO_MAX_US = 2300"),
    }
    with tempfile.TemporaryDirectory(prefix="easycanad-tests-") as directory:
        work = Path(directory)
        for variant, sketch in variants.items():
            print(f"Testing {variant}", flush=True)
            (work / "firmware_under_test.ino").write_text(sketch, encoding="utf-8")
            executable = work / ("firmware_test.exe" if os.name == "nt" else "firmware_test")
            subprocess.run(compiler + [
                "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(ROOT / "tests" / "stubs"), "-I", str(work),
                str(ROOT / "tests" / "firmware_test.cpp"), "-o", str(executable),
            ], check=True)
            for case in CASES:
                # A fresh process resets sketch-local static variables between scenarios.
                subprocess.run([str(executable), case], check=True)
    print(f"All {len(CASES) * len(variants)} scenarios passed.")


if __name__ == "__main__":
    main()
