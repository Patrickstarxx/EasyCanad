# EasyCanad｜鸭翼迎角反馈控制器

EasyCanad 是基于 Arduino Nano 的独立鸭翼控制实验项目：读取风标迎角和接收机输入，计算鸭翼舵机指令。仓库同时保存早期原型，以及正在验证的模块化固件和网页上位机。

> **请选择与代码版本对应的接线和操作说明。** 默认分支 `main` 仍是早期原型；v6 固件与上位机尚在草稿 PR 中。当前没有台架或实机飞行验证结果。

## 代码与验证状态

| 版本 | 内容 | 使用说明 |
| --- | --- | --- |
| [`main`](https://github.com/Patrickstarxx/EasyCanad/tree/main) | 早期单通道 Arduino 原型：A0 风标、D2 俯仰 PWM、D4 标定按钮、D9 舵机；串口 115200 baud | 见下方“默认分支原型” |
| [草稿 PR #1](https://github.com/Patrickstarxx/EasyCanad/pull/1) | 模块化 v6 固件：独立手动/辅助模式、地面配置锁定、故障处理、机型模型、日志回放，以及独立配平和可调迎角补偿 | [固件接线与操作](https://github.com/Patrickstarxx/EasyCanad/blob/codex/fix-firmware-control-safety/FIRMWARE.md) · [台架检查表](https://github.com/Patrickstarxx/EasyCanad/blob/codex/fix-firmware-control-safety/BENCH_TEST_PLAN.md) |
| [草稿 PR #2](https://github.com/Patrickstarxx/EasyCanad/pull/2) | 与 v6 配套的 Windows / Android 网页上位机：设备状态、调参、机型档案、日志曲线和模拟设备 | [上位机说明](https://github.com/Patrickstarxx/EasyCanad/blob/codex/host-configurator/host/README.md) |

PR #2 基于 PR #1；使用上位机连接真实控制器时，应配套相应固件。v6 的软件测试和构建检查已通过，风标方向、舵机行程、失控保护、真实 USB 通信及 Android 兼容性仍需实物验证。模型估算速度不是独立测得的空速。

## 默认分支原型

`main` 中的 [`EasyCanad.ino`](EasyCanad.ino) 是单文件固件。它按约 5 ms 周期读取 A0 风标与 D2 俯仰 PWM，使用 D4 按钮保存风标零位，并从 D9 输出舵机脉宽。其串口调试速率为 **115200 baud**；本段接线和速率不适用于草稿 PR 中的 v6 固件。

| Nano 引脚 | 默认分支用途 |
| --- | --- |
| A0 | 模拟风标迎角输入 |
| D2 | 接收机俯仰 PWM 输入 |
| D4 | 风标零位标定按钮，按下接 GND |
| D9 | 鸭翼舵机信号输出 |

用 Arduino IDE 打开 `EasyCanad.ino`，选择 Arduino Nano（ATmega328P）并上传。舵机应使用符合其负载要求的独立供电，并与 Nano 共地。原型中的参数写在程序内；需要地面配置、日志和 v6 控制律时，请按 PR #1 的固件说明操作。

## 硬件与资料

- [物料清单](BOM.md)与 [`hardware/`](hardware/) 中的原理图、PCB JSON、导入说明和离线包。
- [`cad/`](cad/) 中的风标叶片与主体模型。
- [`archive/`](archive/) 中的早期固件备份。

![EasyCanad PCB 预览](hardware/previewPCB.png)

现有 PCB 尚未为 v6 新增的 D3 模式、D5 地面跳线和 D7 油门接口改版。按 v6 固件接线前，请先核对实际板卡和引线。

## 目前还缺少的验证

- v6 的台架测量：风标角度、鸭翼真实偏角、舵机端点、方向、延迟、故障退出与接收机失控保护。
- Nano 连接上位机后的读取、写入、保存、重启回读，以及长时间日志采集。
- Android 手机、USB 转接器和具体 Nano USB 芯片的串口兼容性；网页模拟设备仅用于软件交互检查。

完成台架检查前，不应把软件测试通过视为飞行验证通过。
