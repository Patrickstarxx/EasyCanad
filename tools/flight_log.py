"""Capture EasyCanad serial logs, inspect configuration, or replay the shared C++ controller."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CONFIG_FIELDS = (
    "gain", "authority_deg", "limit_deg", "filter_tau_us", "slew_deg_s",
    "throttle_idle_scale", "throttle_full_scale", "throttle_tau_us",
    "servo_min_us", "servo_mid_us", "servo_max_us", "servo_direction",
    "rc_direction", "aoa_direction", "throttle_schedule",
)
MODEL_FIELDS = ("model_mass_g", "model_thrust_g", "model_area_dm2", "model_initial_mps", "model_drag")


def read_configuration(line):
    fields = line.strip().split(",")
    if len(fields) != 4 or fields[0] != "C" or fields[1] not in ("4", "5"):
        raise ValueError("not a supported configuration record")
    version = int(fields[1])
    layout = "<8f3H3bB" if version == 4 else "<8f3H3bB4fB"
    values = struct.unpack(layout, bytes.fromhex(fields[2]))
    result = dict(zip(CONFIG_FIELDS + (MODEL_FIELDS if version == 5 else ()), values))
    result["log_version"] = version
    result["zero_deg"] = struct.unpack("<f", bytes.fromhex(fields[3]))[0]
    return result


def capture(args):
    try:
        import serial
    except ImportError as error:
        raise SystemExit("Install capture support: python -m pip install -r tools/requirements.txt") from error
    if args.output.exists():
        raise SystemExit("Output already exists; choose a new file.")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with serial.Serial(args.port, args.baud, timeout=0.25) as connection:
        # Opening a Nano USB port may reset it. Start only after boot; always request metadata.
        time.sleep(2)
        connection.reset_input_buffer()
        connection.write(b"log start\n")
        started = time.monotonic()
        have_config = False
        frames = 0
        with args.output.open("x", encoding="ascii", newline="") as output:
            output.write("#EasyCanadLog\n")
            print("Recording; Ctrl+C stops. Connecting USB can reset the Nano; connect on the ground.")
            try:
                while args.duration == 0 or time.monotonic() - started < args.duration:
                    raw = connection.readline()
                    if not raw.endswith(b"\n"):
                        continue
                    line = raw.decode("ascii", errors="replace").rstrip("\r\n")
                    if not have_config:
                        if not line.startswith(("C,4,", "C,5,")):
                            continue
                        read_configuration(line)
                        have_config = True
                    output.write(line + "\n")
                    if line.startswith("D,"):
                        frames += 1
                    output.flush()
            except KeyboardInterrupt:
                pass
    if not have_config or not frames:
        raise SystemExit("No complete control session captured; check baud, wiring and log start.")
    print(f"Saved {frames} frames to {args.output}")


def replay(args):
    if args.output and args.output.resolve() == args.input.resolve():
        raise SystemExit("Replay output must differ from the input log.")
    compiler = [args.compiler]
    if Path(args.compiler).stem == "zig":
        compiler.append("c++")
    with tempfile.TemporaryDirectory(prefix="easycanad-replay-") as directory:
        executable = Path(directory) / ("replay.exe" if os.name == "nt" else "replay")
        subprocess.run(compiler + ["-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT), str(ROOT / "tools/replay.cpp"), "-o", str(executable)], check=True)
        command = [str(executable), str(args.input)]
        if args.gain is not None:
            command += ["--gain", str(args.gain)]
        if args.servo_dir is not None:
            command += ["--servo-dir", str(args.servo_dir)]
        if args.output:
            with args.output.open("w", encoding="utf-8", newline="") as output:
                return subprocess.run(command, stdout=output).returncode
        return subprocess.run(command).returncode


def inspect(args):
    snapshots = []
    with args.input.open(encoding="ascii") as file:
        for line in file:
            if line.startswith("C,"):
                snapshots.append(read_configuration(line))
    if not snapshots:
        raise SystemExit("No configuration metadata found.")
    print(json.dumps(snapshots, indent=2, ensure_ascii=False, allow_nan=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    cap = commands.add_parser("capture", help="Record serial logs to a new file")
    cap.add_argument("--port", required=True)
    cap.add_argument("--output", type=Path, required=True)
    cap.add_argument("--baud", type=int, default=500000)
    cap.add_argument("--duration", type=float, default=0, help="Seconds; 0 records until Ctrl+C")
    cap.set_defaults(action=capture)
    rep = commands.add_parser("replay", help="Replay using the same controlStep as the firmware")
    rep.add_argument("input", type=Path)
    rep.add_argument("--output", type=Path)
    rep.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    rep.add_argument("--gain", type=float, help="Counterfactual gain; disables comparison with recorded output")
    rep.add_argument("--servo-dir", type=int, choices=(-1, 1))
    rep.set_defaults(action=replay)
    ins = commands.add_parser("inspect", help="Print recorded configuration snapshots")
    ins.add_argument("input", type=Path)
    ins.set_defaults(action=inspect)
    args = parser.parse_args()
    if args.command == "capture" and (args.duration < 0 or args.baud <= 0):
        parser.error("duration must be nonnegative and baud must be positive")
    try:
        return args.action(args) or 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
