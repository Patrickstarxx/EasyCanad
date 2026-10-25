# 设计依据（2026-10-26）

- [NOLOGO SuperMini Nano328P](https://wiki.nologo.tech/product/arduino/superminiNano328P.html)：用户指定模块、电气引脚图及 D13/RGB 共用关系。
- [厂家论坛原始 PCB/STEP 发布帖](https://chat.nologo.tech/d/115)、[厂家下载目录](https://ziliao.nologo.tech/无名科技Nologo资料/Arduino开发板/Supermini%20Nano)：`supermini nano328-WX.PcbDoc` 和 `3D_supermini nano328.step`。提取内侧 23 个通孔，排除半孔，得到 2.54mm 针距、15.46mm 侧排距和 0.289804mm 尾排偏移；两份文件差约 0.000051mm。哈希与逐孔坐标见 `nano_socket_geometry.json`。
- [KiCad 官方 Altium 解析器](https://github.com/KiCad/kicad-source-mirror/blob/master/pcbnew/pcb_io/altium/altium_parser_pcb.cpp)：用于核对 PAD 数据字段；提取工具只读取设计数据。
- [Microchip ATmega328P](https://www.microchip.com/en-us/product/ATMEGA328P)、[厂家数据手册](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU08/ProductDocuments/DataSheets/Atmel-7810-Automotive-Microcontrollers-ATmega328P_Datasheet.pdf)：5V 逻辑和输入门限，不能把供电稳压等同于 SPI 电平转换。
- [Pololu 2587](https://www.pololu.com/product/2587)、[原理图下载页](https://www.pololu.com/product/2587/resources)：5V 供电、完整逻辑兼容，厂家明确说明 MISO/DO 转为 5V；CD 在插卡时为高，无卡接地。载板采用这类外接模块接口，不连接模块的 3V3 输出。
- [AO4407A](https://www.aosmd.com/sites/default/files/res/datasheets/AO4407A.pdf)：输入反接保护；源极为保护后总线，漏极接 J3 电源入口。
- [OMRON B3F](https://omronfs.omron.com/en_US/ecb/products/pdf/en-b3f.pdf)：SW1 触点 1/2 与 3/4 分组，按下接通。
- [嘉立创 EDA CLI](https://prodocs.lceda.cn/cn/api/guide/cli.html)：使用本机专业版 4.1.78 创建、导出和检查。
- [Freerouting 2.1.0](https://github.com/freerouting/freerouting/releases/tag/v2.1.0)：基于实际焊盘的本地辅助布线；最终以 EDA DRC 和导出文件核对为准。

PCB 尺寸、分区、J3 唯一供电入口、J4/J5 中心针悬空、外接 TF 模块接口为本版工程决定。没有改动固件。

- Gerbonara 与 resvg-py：只用于将已导出的生产 Gerber 渲染为 SVG/PNG 预览，不参与网表或 PCB 电路修改；原生编辑器渲染接口在本机返回 1×1 占位图，因此交付预览直接使用加工数据。
