"""Compile the real sketch against hardware doubles, then run isolated scenarios."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "startup", "legacy", "record", "power-loss", "rc-capture", "rc-rollover", "aux-capture",
    "rc-loss-output", "mapping", "manual-priority", "sensor", "manual-fallback", "direction-rate",
    "mode", "mode-loss", "throttle", "directions", "ground-lock", "ground-run", "parameters",
    "config-persistence", "config-power-loss", "calibration-seam", "calibration-reject",
    "calibration-integration", "button", "telemetry", "speed-history",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    compiler = [args.compiler]
    if Path(args.compiler).stem == "zig":
        compiler.append("c++")
    source = (ROOT / "EasyCanad.ino").read_text(encoding="utf-8")
    core = (ROOT / "ControlCore.h").read_text(encoding="utf-8")
    variants = {
        "default": core,
        "limited-asymmetric": core.replace("1.2f, 90, 90,", "1.2f, 90, 30,")
            .replace("500, 1500, 2500,", "700, 1400, 2300,"),
    }
    with tempfile.TemporaryDirectory(prefix="easycanad-tests-") as directory:
        work = Path(directory)
        replay = work / ("replay.exe" if os.name == "nt" else "replay")
        subprocess.run(compiler + ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
            str(ROOT / "tools/replay.cpp"), "-o", str(replay)], check=True)
        for variant, variant_core in variants.items():
            print(f"Testing {variant}", flush=True)
            (work / "firmware_under_test.ino").write_text(source, encoding="utf-8")
            for header in ROOT.glob("*.h"):
                (work / header.name).write_text(header.read_text(encoding="utf-8"), encoding="utf-8")
            (work / "ControlCore.h").write_text(variant_core, encoding="utf-8")
            executable = work / ("firmware_test.exe" if os.name == "nt" else "firmware_test")
            subprocess.run(compiler + [
                "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(ROOT / "tests" / "stubs"), "-I", str(work),
                str(ROOT / "tests" / "firmware_test.cpp"), "-o", str(executable),
            ], check=True)
            for case in CASES:
                # A fresh process resets sketch-local static variables between scenarios.
                subprocess.run([str(executable), case], check=True)
            log = work / f"{variant}.csv"
            subprocess.run([str(executable), "emit-log", str(log)], check=True)
            result = subprocess.run([str(replay), str(log)], check=True, capture_output=True, text=True)
            assert "mismatches=0" in result.stderr
            assert "model_unobserved" in result.stdout
            print(f"PASS actual firmware log replay: {result.stderr.strip()}", flush=True)
            override = subprocess.run([str(replay), str(log), "--gain", "0.8"],
                check=True, capture_output=True, text=True)
            assert "comparison=off" in override.stderr and "changed" in override.stdout
            lines = log.read_text(encoding="utf-8").splitlines()
            first_data = next(i for i, line in enumerate(lines) if line.startswith("D,"))
            corrupted = lines.copy()
            fields = corrupted[first_data].split(",")
            fields[11] = str(int(fields[11]) + 10)
            corrupted[first_data] = ",".join(fields)
            bad = work / "bad.csv"
            bad.write_text("\n".join(corrupted) + "\n", encoding="utf-8")
            assert subprocess.run([str(replay), str(bad)], capture_output=True).returncode == 1
            without_state = [line for line in lines if not line.startswith("R,")]
            bad.write_text("\n".join(without_state) + "\n", encoding="utf-8")
            assert subprocess.run([str(replay), str(bad)], capture_output=True).returncode == 2
            removed = next(i for i in range(first_data + 3, len(lines) - 1)
                           if lines[i].startswith("D,") and lines[i + 1].startswith("D,"))
            bad.write_text("\n".join(lines[:removed] + lines[removed + 1:]) + "\n", encoding="utf-8")
            assert subprocess.run([str(replay), str(bad)], capture_output=True).returncode == 2
            print("PASS replay overrides, tamper detection and missing-frame rejection", flush=True)
            if variant == "default":
                subprocess.run([sys.executable, str(ROOT / "tools/flight_log.py"), "inspect", str(log)],
                               check=True, capture_output=True)
                exported = work / "replay-export.csv"
                subprocess.run([sys.executable, str(ROOT / "tools/flight_log.py"), "replay", str(log),
                    "--compiler", args.compiler, "--output", str(exported)], check=True)
                assert "gain_source" in exported.read_text(encoding="utf-8")
                print("PASS capture-tool inspect/replay command-line integration", flush=True)
    subprocess.run([sys.executable, "-B", str(ROOT / "tests/test_flight_log.py")], check=True)
    subprocess.run([sys.executable, str(ROOT / "tools/model_profiles.py"), "check",
                    str(ROOT / "profiles/example.json")], check=True, capture_output=True)
    print(f"All {len(CASES) * len(variants)} scenarios passed.")


if __name__ == "__main__":
    main()
