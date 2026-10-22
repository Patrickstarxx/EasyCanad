"""Per-airframe model profiles: desktop editor, JSON files and ground-only upload."""
import argparse
from dataclasses import asdict, dataclass
import json
import math
from pathlib import Path
import queue
import threading
import time

from flight_log import read_configuration

DRAG_LEVELS = {"low": 1, "medium": 2, "high": 3}


@dataclass(frozen=True)
class ModelProfile:
    name: str
    mass_g: float
    static_thrust_g: float
    wing_area_dm2: float
    drag: str = "medium"
    initial_speed_mps: float = 0
    use_model_gain: bool = False

    def validate(self):
        if not self.name.strip():
            raise ValueError("请填写机型名称")
        ranges = (("起飞重量 g", self.mass_g, 10, 20000),
                  ("静推力 g", self.static_thrust_g, 0.001, 50000),
                  ("翼面积 dm²", self.wing_area_dm2, 0.1, 500),
                  ("初始速度 m/s", self.initial_speed_mps, 0, 100))
        for label, value, minimum, maximum in ranges:
            if not math.isfinite(value) or not minimum <= value <= maximum:
                raise ValueError(f"{label}应在 {minimum}～{maximum} 之间")
        if self.drag not in DRAG_LEVELS:
            raise ValueError("阻力档位应为 low、medium 或 high")
        if not isinstance(self.use_model_gain, bool):
            raise ValueError("use_model_gain 应为 true 或 false")

    @property
    def thrust_to_weight(self):
        return self.static_thrust_g / self.mass_g

    def commands(self):
        self.validate()
        return ["set throttle_schedule 0", f"set model_mass_g {self.mass_g:.7g}",
                f"set model_thrust_g {self.static_thrust_g:.7g}",
                f"set model_area_dm2 {self.wing_area_dm2:.7g}",
                f"set model_drag {DRAG_LEVELS[self.drag]}",
                f"set model_initial_mps {self.initial_speed_mps:.7g}",
                f"set throttle_schedule {2 if self.use_model_gain else 0}", "save"]

    def save(self, path):
        self.validate()
        path.write_text(json.dumps({"format": "easycanad-model", "version": 1,
                                    **asdict(self)}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    @classmethod
    def load(cls, path):
        data = json.loads(path.read_text(encoding="utf-8"))
        if data.pop("format") != "easycanad-model" or data.pop("version") != 1:
            raise ValueError("不支持的机型配置格式")
        profile = cls(**data)
        profile.validate()
        return profile


def wait_event(device, expected):
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        line = device.readline().decode("ascii").strip()
        if not line.startswith("E,"):
            continue
        event = line.split(",")[1]
        if event != expected:
            raise RuntimeError(f"设备回复 {event}，需要 {expected}；请检查地面跳线和固件版本")
        return
    raise RuntimeError("等待设备回复超时")


def upload_profile(profile, port):
    import serial
    profile.validate()
    # 打开 USB 可能使 Nano 复位；必须在地面、D5 接地时操作。
    with serial.Serial(port, 500000, timeout=0.2) as device:
        time.sleep(2)
        device.reset_input_buffer()
        device.write(b"status\n")
        wait_event(device, "GROUND")
        device.write(b"log stop\n")
        wait_event(device, "LOG_OFF")
        for command in profile.commands():
            device.write((command + "\n").encode("ascii"))
            wait_event(device, "SAVED" if command == "save" else "SET_UNSAVED")
        device.write(b"status\n")
        wait_event(device, "GROUND")
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            line = device.readline().decode("ascii").strip()
            if line.startswith("C,"):
                settings = read_configuration(line)
                expected = {"model_mass_g": profile.mass_g, "model_thrust_g": profile.static_thrust_g,
                            "model_area_dm2": profile.wing_area_dm2,
                            "model_initial_mps": profile.initial_speed_mps,
                            "model_drag": DRAG_LEVELS[profile.drag],
                            "throttle_schedule": 2 if profile.use_model_gain else 0}
                if any(not math.isclose(settings[key], value, rel_tol=1e-5, abs_tol=1e-5)
                       for key, value in expected.items()):
                    raise RuntimeError("设备回读参数与机型配置不一致")
                device.write(b"log start\n")
                wait_event(device, "LOG_ON")
                return settings
        raise RuntimeError("保存已回复，未收到配置回读")


def launch_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("EasyCanad 机型参数")
    root.minsize(680, 620)
    panel = ttk.Frame(root, padding=20)
    panel.pack(fill="both", expand=True)
    panel.columnconfigure(1, weight=1)
    ttk.Label(panel, text="每个机型一份配置", font=("Microsoft YaHei UI", 15)).grid(
        row=0, column=0, columnspan=2, sticky="w", pady=(0, 14))
    values = {}
    fields = (("name", "机型名称", ""), ("mass_g", "起飞重量（g，含电池）", ""),
              ("static_thrust_g", "满油门静推力（g）", ""), ("wing_area_dm2", "参考翼面积（dm²）", ""))
    for row, (key, label, default) in enumerate(fields, start=1):
        values[key] = tk.StringVar(value=default)
        ttk.Label(panel, text=label).grid(row=row, column=0, sticky="w", pady=6)
        ttk.Entry(panel, textvariable=values[key]).grid(row=row, column=1, sticky="ew", padx=(16, 0))
    drag = tk.StringVar(value="中阻")
    ttk.Label(panel, text="阻力档位（粗略模型假设）").grid(row=5, column=0, sticky="w", pady=6)
    ttk.Combobox(panel, textvariable=drag, values=("低阻", "中阻", "高阻"), state="readonly").grid(row=5, column=1, sticky="ew", padx=(16, 0))
    initial = tk.StringVar(value="0")
    ttk.Label(panel, text="可选：首次辅助时初始速度（m/s）").grid(row=6, column=0, sticky="w", pady=6)
    ttk.Entry(panel, textvariable=initial).grid(row=6, column=1, sticky="ew", padx=(16, 0))
    schedule = tk.BooleanVar(value=False)
    ttk.Checkbutton(panel, text="将模型预测用于增益调度（默认仅记录预测）", variable=schedule).grid(
        row=7, column=0, columnspan=2, sticky="w", pady=12)
    ttk.Label(panel, text="无姿态、GPS 或空速观测；无法识别俯冲、阵风或被手持约束。\n初始速度 0 仅适用于从静止开始；不能据此判断失速。",
              wraplength=600).grid(row=8, column=0, columnspan=2, sticky="w", pady=8)
    status = tk.StringVar(value="填写四项基础参数，保存为 JSON 后可用于其他机型。")

    def current():
        p = ModelProfile(values["name"].get(), float(values["mass_g"].get()),
                         float(values["static_thrust_g"].get()), float(values["wing_area_dm2"].get()),
                         {"低阻": "low", "中阻": "medium", "高阻": "high"}[drag.get()],
                         float(initial.get()), schedule.get())
        p.validate()
        return p

    def save():
        try:
            p = current()
            path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("机型配置", "*.json")])
            if path:
                p.save(Path(path))
                status.set(f"已保存；推重比 {p.thrust_to_weight:.2f}，模型假定近似水平飞行。")
        except ValueError as error:
            messagebox.showerror("参数", str(error))

    def load():
        path = filedialog.askopenfilename(filetypes=[("机型配置", "*.json")])
        if not path:
            return
        try:
            p = ModelProfile.load(Path(path))
            for key in values:
                values[key].set(str(getattr(p, key)))
            drag.set({"low": "低阻", "medium": "中阻", "high": "高阻"}[p.drag])
            initial.set(str(p.initial_speed_mps))
            schedule.set(p.use_model_gain)
            status.set(f"已载入 {p.name}；推重比 {p.thrust_to_weight:.2f}")
        except (ValueError, OSError, KeyError, TypeError) as error:
            messagebox.showerror("配置文件", str(error))

    buttons = ttk.Frame(panel)
    buttons.grid(row=9, column=0, columnspan=2, sticky="w", pady=12)
    ttk.Button(buttons, text="载入机型", command=load).pack(side="left", padx=(0, 8))
    ttk.Button(buttons, text="保存机型", command=save).pack(side="left")
    port = tk.StringVar(value="COM5")
    ttk.Label(panel, text="地面串口（D5 上电前接 GND）").grid(row=10, column=0, sticky="w", pady=6)
    ttk.Entry(panel, textvariable=port).grid(row=10, column=1, sticky="ew", padx=(16, 0))
    results = queue.Queue()

    def apply():
        try:
            p = current()
        except ValueError as error:
            messagebox.showerror("参数", str(error))
            return
        target = port.get().strip()
        apply_button.state(["disabled"])
        status.set("连接地面配置，写入、保存并回读；不会发送 run。")

        def worker():
            try:
                upload_profile(p, target)
                results.put((True, "已保存并回读一致；设备仍在地面配置状态。"))
            except (OSError, RuntimeError, ImportError, ValueError, KeyError) as error:
                results.put((False, str(error)))
        threading.Thread(target=worker, daemon=True).start()

    def poll():
        try:
            ok, message = results.get_nowait()
        except queue.Empty:
            root.after(100, poll)
            return
        apply_button.state(["!disabled"])
        status.set(message)
        if not ok:
            messagebox.showerror("写入", message)
        root.after(100, poll)

    apply_button = ttk.Button(panel, text="写入并保存到地面设备", command=apply)
    apply_button.grid(row=11, column=0, columnspan=2, sticky="w", pady=12)
    ttk.Label(panel, textvariable=status, wraplength=600).grid(row=12, column=0, columnspan=2, sticky="w", pady=8)
    root.after(100, poll)
    root.mainloop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command")
    commands.add_parser("gui")
    check = commands.add_parser("check")
    check.add_argument("profile", type=Path)
    apply = commands.add_parser("apply")
    apply.add_argument("profile", type=Path)
    apply.add_argument("--port", required=True)
    args = parser.parse_args()
    if args.command in (None, "gui"):
        launch_gui()
        return
    profile = ModelProfile.load(args.profile)
    if args.command == "check":
        print(f"{profile.name}: thrust/weight={profile.thrust_to_weight:.3f}")
        print("\n".join(profile.commands()))
    else:
        upload_profile(profile, args.port)
        print("Saved and verified; controller remains in ground configuration.")


if __name__ == "__main__":
    main()
