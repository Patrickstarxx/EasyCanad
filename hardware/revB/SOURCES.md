# 设计依据与取舍

查阅日期：2026-10-25。

- 用户指定模块：[NOLOGO SuperMini Nano328P](https://wiki.nologo.tech/product/arduino/superminiNano328P.html)。采用其引脚图的三排排列；不使用标准 Nano V3 的 2×15 封装。厂家图片标出 23.50×18.00mm 外形及 20.32mm 的八个针距跨度，但未完整标注第三排与两侧排针的全部孔中心关系。因此本版使用三条编号短线束连接模块，并将模块独立固定，不把图片测量值当成已验证的插座孔距。
- [TI SN74LVC125A 数据手册](https://www.ti.com/lit/ds/symlink/sn74lvc125a.pdf)：U3 在 3.3V 工作，输入支持 5.5V，用于 CS/MOSI/SCK 降压。第四路输入接地、输出不接、使能拉高。
- [TI SN74AHCT125 数据手册](https://www.ti.com/lit/ds/symlink/sn74ahct125.pdf)：U4 使用 5V 供电和 TTL 输入阈值，用于接收机的 3.3/5V PWM 及 TF MISO 升压。MISO 的 /OE 由 5V 侧 CS 控制，其余三路常开。
- [Diodes AP7361C 数据手册](https://www.diodes.com/datasheet/download/AP7361C.pdf)：独立 3.3V LDO，SOT-223，1A 器件额定值；选用可适配低 ESR 陶瓷输出电容的型号。整板 TF 可用电流仍受散热、BEC、线束及卡本身影响，不能直接等同于 1A。
- [AOS AO4407A](https://www.aosmd.com/sites/default/files/res/datasheets/AO4407A.pdf)：P 沟道反接保护，漏极接输入，源极接受保护总线。PCB/连接器没有按 MOSFET 数据手册的最大电流额定整板。
- [OMRON B3F](https://omronfs.omron.com/en_US/ecb/products/pdf/en-b3f.pdf)：SW1 的 1–2 和 3–4 分别为同一常通触点组，按下使两组导通。
- [SD Association 简化规范入口](https://www.sdcard.org/downloads/pls/)：TF 接口按 3.3V SPI 接入，使用无源卡座转接板；外部适配板需要补齐 DAT1/DAT2 上拉和近端去耦。
- [嘉立创 EDA 命令行接口](https://prodocs.lceda.cn/cn/api/guide/cli.html)：使用已安装 4.1.78 专业版创建、放置、连接、导出及检查。
- [Freerouting](https://github.com/freerouting/freerouting/releases/tag/v2.1.0)：本地信号布线回退工具。由实际 EDA 焊盘/电源线快照构造 DSN，布线完成后在 EDA 内重新检查；不把路由器的完成状态替代为硬件验证。

电源路径、模块线束以及 TF 外部卡座约定是本版工程决定，均已写入接线说明；未更改控制算法或固件。
