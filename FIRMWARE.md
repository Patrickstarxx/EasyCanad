# EasyCanad 固件 v4：模式切换、配置锁定与日志回放

当前程序使用攻角与接收机 PWM 控制鸭翼，支持手动/辅助切换、地面配置锁定、独立方向反转和油门代理增益调度。油门调度只反映油门输入趋势，**不输出估计空速，也不用于失速判断**。v3 有效攻角零位可继续使用；v1/v2 的旧零位仍需重新标定。

## 接线

| Nano 引脚 | 用途 | 接法 |
|---|---|---|
| A0 | 攻角传感器 | 原有模拟输入，0～5V 对应 360° |
| D2 / INT0 | 俯仰通道 | 原有接收机 PWM 输入 |
| **D3 / INT1** | **手动/辅助开关** | 新增一路接收机 PWM：1000μs 手动，2000μs 辅助 |
| D4 | 零位标定按钮 | 原有按钮，按下接 GND |
| **D5** | **地面配置跳线** | 新增两针跳线；上电前将 D5 接 GND 才能进入配置状态 |
| **D7 / PCINT23** | **油门通道** | 新增接收机油门 PWM，1000μs 对应低油门，2000μs 对应高油门 |
| D9 | 鸭翼舵机 | 原有输出 |
| D0/D1 或 USB | 配置与日志 | 串口 **500000 baud、8N1**；USB 已连接串口，无需另接 D0/D1 |

接收机、传感器、Arduino 和舵机信号地共地。新增输入不要直接接入超过 Nano 允许范围的电平。现有 PCB 没有新增接口，需要从 Nano 对应引脚引线；本次没有修改 PCB 文件。舵机按实际电流选择供电并校准端点。

D7 使用 AVR 引脚变化中断；D3 使用外部中断，均不阻塞等待脉宽。三个 PWM 通道分别测量和超时，不会互相代替有效信号。

## 地面配置和运行锁定

**普通启动：D5 悬空，上电直接进入 RUN。** RUN 期间禁止标定、改方向、改参数、保存配置或恢复默认值。运行中再将 D5 接地也不会打开配置。

需要配置时：

1. 在地面断电，将 D5 与 GND 短接，再上电。进入 GROUND，舵机保持中位。
2. 接通接收机，模式开关置手动，俯仰杆回中。打开 500000 baud 串口，发送命令，每条以换行结束。
3. 完成方向、增益、限幅和零位校准。参数修改立即作用于本次配置会话，发送 `save` 才会持久保存。
4. 拔掉 D5 跳线。保持有效俯仰/模式输入、手动模式和俯仰中位，发送 `run`。
5. 程序回复 `E,RUN_LOCKED,...`。本次上电内无法重新进入配置；需要再次在地面重新上电。

拔掉跳线本身不会结束 GROUND，必须显式发送 `run`。正在标定时不能修改配置或进入运行。D5 跳线是地面操作约束，不是飞行状态检测器；在地面完成接线和配置后再使用。

打开 USB 串口可能触发 Nano 自动复位，应在地面连接。软件手动模式仍依赖主控、输入捕获和舵机输出正常工作，不提供主控故障时的硬件旁路。

## 标定

只有 GROUND 状态、模式通道有效且处于手动、俯仰输入有效并在 ±10μs 中位死区内时，长按 D4 才能启动标定。

1. 固定风标到所需零攻角参考方向；无气流时，仅摆正机身不代表风标已经对准。
2. 长按按钮至少 1.5 秒，随后保持风标稳定约 160ms。
3. 程序采集 32 个样本，以首样本展开圆周接缝后平均，179°/-179°附近不会被错误平均成 0°。
4. 采样总波动超过 2°、遥控通道超时或手动/中位条件不再满足，本次标定被拒绝。
5. 日志 `cal_status=2` 表示保存并回读校验成功；3 表示拒绝/失败。继续按住按钮不会重复标定。

攻角零位记录位于 EEPROM 地址 16，保留 v3 格式、版本、映射参数和 CRC16。写入先失效、最后提交，成功后清除控制滤波状态。攻角零位写入中断可能要求重新标定。新增配置使用地址 64 和 192 的双槽记录，包含序号和 CRC；写入新槽中断时，上次完整配置仍可加载。

## 手动、辅助及异常行为

模式通道达到 1700μs 并连续有效约 100ms 后，允许进入辅助；低于 1700μs，包括开关中间位置，选择手动。标定/进入 RUN 的手动判定更严格，要求不超过 1300μs。

辅助权重约 300ms 渐入/渐出，最终舵令同时受变化速率限制。俯仰满杆时优先保留手动权限，自动补偿使用剩余的对称舵量预算：

```text
manual = clamp(-normalizeRC × rc_dir × authority, ±limit)
automatic = clamp(-gain × throttleScale × assistBlend × aoa_dir × filteredAOA,
                  ±(limit - abs(manual)))
target = clamp(manual + automatic, ±limit)
```

正负逻辑角度映射到舵机最小/最大端点前，再应用 `servo_dir`。将机械限幅从 90°减至 30°不会把这 30°重新放大成整个舵机行程。

| state | 名称 | 行为 |
|---|---|---|
| 0 | UNCALIBRATED | 请求辅助但无有效零位：保留手动，关闭自动补偿 |
| 1 | RC_LOST | 俯仰 PWM 未收到/100ms 超时：立即回中并重置控制状态 |
| 2 | CALIBRATING | 标定过程中及完成/中断的当次周期：保持中位 |
| 3 | SENSOR_FAULT | 请求辅助但攻角异常或恢复确认中：保留手动 |
| 4 | ACTIVE | 辅助有效，补偿按权重和调度增益渐入 |
| 5 | MANUAL | 手动；切换后的短时间内辅助平滑退出 |
| 6 | MODE_LOST | 模式 PWM 未收到/超时：退到手动并撤销辅助目标 |
| 7 | GROUND_CONFIG | 地面配置状态：保持中位 |
| 8 | THROTTLE_LOST | 启用油门调度但油门未收到/超时：退到手动 |

与 v3 不同，未标定时仍能使用手动模式。辅助模式必须有有效零位。攻角检查包括相对角度 ±90°、相邻样本变化不超过 45°；异常后连续 10 个合理样本才恢复。有效但错误的传感器读数不能仅靠这些检查全部识别。

接收机无线失联后如果仍输出正常 PWM，单靠脉宽无法判断链路已经失联，应同时配置接收机自身的失控保护。

## 油门代理增益调度

没有空速传感器或其他可观测速度的数据，无法从攻角、俯仰杆和油门唯一推算真实空速。因此这里使用可配置的油门调度，日志来源标记 `throttle_proxy`，没有任何 m/s 数值。

```text
throttleNorm = clamp((throttlePWM - 1000) / 1000, 0, 1)
throttleFiltered += dt / (throttleTau + dt) × (throttleNorm - throttleFiltered)
throttleScale = idleScale + (fullScale - idleScale) × throttleFiltered
effectiveGain = gain × throttleScale × assistBlend
```

默认低油门系数 1、高油门系数 0.5、油门滤波时间 500ms，基础增益 1.2。参数限制 `0 ≤ fullScale ≤ idleScale ≤ 1`，调度最多降低基础增益。油门和空速在滑翔、俯冲、逆风等情况下可能差别很大，这些默认值需要用实际机体试验调整。

若不使用油门调度，在 GROUND 发送 `set throttle_schedule 0` 和 `save`，辅助改用固定基础增益，D7 可不接。启用时 D7 超时会关闭辅助，手动继续可用。油门通道应提供低油门 1000μs、高油门 2000μs 的方向。

## 配置命令

`status`、`log start`、`log stop` 在 RUN 也可用。其他命令只允许在 GROUND、未标定采样时使用。

| 命令 | 含义与范围 |
|---|---|
| `set gain 1.2` | 基础攻角增益，0～5 |
| `set authority 90` | 遥控满杆权限，0～90° |
| `set limit 30` | 实际机械限幅，(0, 90]° |
| `set servo_dir -1` | 反转整个舵机输出；仅允许 ±1 |
| `set rc_dir -1` | 仅反转遥控方向；仅允许 ±1 |
| `set aoa_dir -1` | 仅反转攻角补偿方向；仅允许 ±1 |
| `set servo_min 700` | 最小端点，500～2500μs，须为 4μs 的整数倍 |
| `set servo_mid 1500` | 中位，整数 μs |
| `set servo_max 2300` | 最大端点，500～2500μs，须为 4μs 的整数倍 |
| `set filter_ms 28.333333` | 攻角滤波时间，1～1000ms |
| `set slew 360` | 指令变化速率，10～1000°/s |
| `set throttle_schedule 1` | 1 油门调度，0 固定增益 |
| `set throttle_idle 1` | 低油门系数，0～1 |
| `set throttle_full 0.5` | 高油门系数，0～低油门系数 |
| `set throttle_tau_ms 500` | 油门滤波时间，10～5000ms |
| `save` | 把当前配置保存到下一个 EEPROM 槽 |
| `defaults` | 本次会话恢复默认参数，尚未保存；不清除攻角零位 |
| `run` | 拔掉跳线且满足手动/中位条件后进入锁定运行状态 |

每次参数变更都整体检查端点和范围，要求 `servo_min < servo_mid < servo_max`。非整数方向、NaN、越界值和未知参数都会拒绝。指令行最多 63 个字符，超长命令整行丢弃。

回复为 `E,事件,cal_status`。例如 `SET_UNSAVED` 表示本次修改成功但尚未保存；`SAVED` 表示持久保存完成；`ERR_LOCKED` 表示运行状态禁止写入。状态命令还会输出一条 C 配置记录；用下方的 `inspect` 将其解码。

## 记录、检查和回放日志

使用电脑或连接 D0/D1 的外部串口记录设备保存日志。当前 Nano **没有板载飞行日志存储**，记录端需要在整个采集期间连接；USB 适合台架，飞行采集需外部记录器或合适的串口数传。

安装采集端依赖：

```sh
python -m pip install -r tools/requirements.txt
```

采集 60 秒（COM5 换成实际串口），或者省略 duration 后按 Ctrl+C 结束：

```sh
python tools/flight_log.py capture --port COM5 --duration 60 --output logs/flight.ecsv
python tools/flight_log.py inspect logs/flight.ecsv
```

回放需要主机 C++11 编译器（g++、clang++ 或 Zig），不连接舵机，不向 Arduino 发送回放指令：

```sh
python tools/flight_log.py replay logs/flight.ecsv --output logs/replay.csv
python tools/flight_log.py replay logs/flight.ecsv --gain 0.8 --output logs/gain-08.csv
# 可追加 --compiler /path/to/g++，或 --compiler /path/to/zig.exe
```

不覆盖参数时，回放核对状态、指令角度、舵机脉宽、有效增益、混合权重及增益来源；允许主机与 AVR 浮点计算产生约 1μs / 0.02°的舍入差别。参数覆盖用于比较算法输出，关闭与原舵令的一致性判定。**这是已记录输入的控制器回放，不模拟改变舵令之后飞机、气流和传感器会如何变化。**

日志拥塞时丢帧并累计计数，控制循环继续运行。下一条可发送的数据前会附上完整算法状态快照，回放在此重同步；缺失区间本身无法恢复。若文件丢失了帧且没有对应快照，回放报错而不会把结果当作完整验证。修改增益后的对比也会在快照处恢复记录时的动态状态，跨缺失区间不能视为连续仿真。

### 原始记录格式 v4

- `C,4,configHex,zeroHex`：小端二进制十六进制配置和攻角零位，避免浮点文本丢失精度。配置布局为 `<8f3H3bB`（42 字节）。
- `R,memoryHex`：下一帧计算之前的控制器状态，布局 `<5fI3B`（27 字节）。
- `E,event,cal_status`：命令事件。
- `D,...`：每个 5ms 控制周期尝试记录一次，字段如下：

```text
D,seq,t_us,dt_us,aoa_adc,rc_us,mode_us,throttle_us,flags,state,cmd_cdeg,
servo_us,gain_milli,blend_milli,gain_source,drop,miss,cal_status,max_interval_us
```

上述字段在实际文件中为单行。角度单位 0.01°；增益和权重单位 0.001。增益来源：0 无补偿、1 固定、2 油门代理。

flags 位：1 俯仰有效、2 模式有效、4 油门有效、8 攻角零位有效、16 地面配置、32 标定中。原始 PWM 可能保留最后一次值，是否有效以 flags 为准。

`seq` 每个控制周期递增，暂停日志期间也递增；`drop` 只统计日志启用时未能记录的帧。`miss` 为未执行的完整 5ms 周期数，`max_interval_us` 为最长实际控制间隔；标定和配置保存时间也会反映在统计中。

## 验证

控制算法集中在 `ControlCore.h`，固件和 `tools/replay.cpp` 调用同一个 `controlStep`。主机测试编译实际草图，覆盖 PWM 超时/回绕、模式确认与淡入淡出、手动降级、配置锁定、三种方向、油门调度、双槽配置断电、标定和串口拥塞。用默认配置以及 ±30°限幅、非对称端点重复执行，并从固件实际日志回放、检查丢帧重同步、输出篡改和缺帧报错。

```sh
python tests/run_tests.py
# 或 python tests/run_tests.py --compiler /path/to/compiler
```

GitHub Actions 运行同样的行为/回放检查，并编译 Nano 新旧引导程序配置。依赖 `arduino:avr@1.8.6` 和 `Servo@1.3.0`。这些验证不代替实际机体的控制方向、端点、调度系数和异常行为测试。