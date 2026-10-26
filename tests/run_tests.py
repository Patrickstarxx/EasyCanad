"""Compile the actual single-file sketch with hardware doubles; no board required."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ("math", "mapping", "limits", "wrap", "calibration", "rc", "rollover", "telemetry")
VARIANTS = {
    "K0": {"AOA_COMPENSATION_K": "0.0f"},
    "K05": {},
    "K1": {"AOA_COMPENSATION_K": "1.0f"},
    "reference5": {"AOA_REFERENCE_DEG": "5.0f"},
    "trim3": {"CANARD_TRIM_DEG": "3.0f"},
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    compiler = [args.compiler]
    if Path(args.compiler).stem == "zig":
        compiler.append("c++")
    source = (ROOT / "EasyCanad.ino").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="easycanad-linear-tests-") as directory:
        work = Path(directory)
        executable = work / ("test.exe" if os.name == "nt" else "test")
        for name, overrides in VARIANTS.items():
            variant = source
            for key, value in overrides.items():
                variant, count = re.subn(rf"(const float {key} = )[^;]+;", rf"\g<1>{value};", variant)
                assert count == 1, key
            (work / "firmware_under_test.ino").write_text(variant, encoding="utf-8")
            subprocess.run(compiler + ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(ROOT / "tests/stubs"), "-I", str(work),
                str(ROOT / "tests/firmware_test.cpp"), "-o", str(executable)], check=True)
            for case in CASES:
                subprocess.run([str(executable), case], check=True)
            print(f"PASS {name}: {len(CASES)} scenarios", flush=True)
    print(f"PASS all {len(VARIANTS) * len(CASES)} isolated scenarios")


if __name__ == "__main__":
    main()
