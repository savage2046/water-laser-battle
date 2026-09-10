<<<<<<< .mine
# main 板 ESP32-S3 引脚重映射方案（gun-board.eprj2 / main）

> 日期：2026-08-22 · 状态：**提案待确认**
> 背景：main 板主控定为 **LCKFB-ESP32S3R8N8 开发板（载板方案）**，但 gun 固件
> （firmware/gun）原引脚表按 **ESP32 经典款**编写（GPIO25/12/26/36/27/32/15/14/4/33/
> 5/18/23/19/16/17/21/22/34/35/0/2/13/39），**与 ESP32-S3 不兼容**（S3 无 GPIO36/39
> 等），必须整体重映射。本表是 PCB 载板布线 + 固件 config.h 修改的共同依据。

## 1. ESP32-S3R8N8 引脚硬约束（不可用作普通 IO）

| 引脚 | 占用 | 说明 |
| --- | --- | --- |
| GPIO26–32 | SPI Flash（SPI0/1） | S3R8N8 板载 8MB Flash 占用，禁用 |
| GPIO33–37 | **Octal PSRAM**（8MB） | R8 = Octal PSRAM 专用，禁用 |
| GPIO19 / GPIO20 | USB D- / D+ | 开发板 USB-C 占用（烧录/串口），禁用 |
| GPIO0 | BOOT 键 / strapping | 保留给板载 BOOT（下载模式），不占用 |
| GPIO3 | JTAG 信号源 strapping | 上电电平敏感，避免使用 |
| GPIO45 / GPIO46 | VDD_SPI 电压 strapping | 避免使用 |
| GPIO48 | 板载 LED | 保留（状态灯可用，但不占用） |

**实际可用 GPIO 池**：GPIO1–18、GPIO21、GPIO38–44、GPIO47（约 26 个）

## 2. 功能 → S3 GPIO 映射表（提案）

| 功能 | S3 GPIO | 方向 | 说明（对齐 firmware/gun/src/config.h 语义） |
| --- | --- | --- | --- |
| IR_TX（940nm 38kHz） | **GPIO1** | 输出 | 载波+数据，经驱动管接 940nm LED |
| IR_TX_850（56kHz） | **GPIO2** | 输出 | 接 850nm LED（S3 无 GPIO12 strapping 问题） |
| IR_RX（940nm TSOP38238） | **GPIO4** | 输入/中断 | 空闲高电平 |
| IR_RX_850（TSOP4856） | **GPIO5** | 输入/中断 | 空闲高电平 |
| TRIGGER（扳机） | **GPIO6** | 输入(上拉) | 微动开关按下接地 |
| MOTOR（电机 MOSFET） | **GPIO7** | 输出 | AO3400 栅极，扣扳机联动 |
| LED_DATA（WS2812） | **GPIO8** | 输出 | 状态灯（RMT 驱动） |
| I2S_BCLK | **GPIO9** | 输出 | MAX98357A（S3 GPIO matrix 可任意映射） |
| LIGHT_SENSE（环境光） | **GPIO10** | ADC1_CH10 | 光敏二极管+分压（S3 的 ADC1=GPIO1-10） |
| I2S_DOUT | **GPIO11** | 输出 | MAX98357A DIN |
| SX_SCLK | **GPIO12** | SPI 时钟 | E22-400M22S |
| SX_MISO | **GPIO13** | SPI MISO | E22-400M22S |
| SX_MOSI | **GPIO14** | SPI MOSI | E22-400M22S |
| SX_NSS | **GPIO15** | SPI CS | E22-400M22S |
| SX_RST | **GPIO16** | 输出 | E22-400M22S NRST（低有效复位） |
| SX_BUSY | **GPIO17** | 输入 | E22-400M22S GPIO4(BUSY) |
| I2S_WS | **GPIO18** | 输出 | MAX98357A LRCK |
| OLED_SDA | **GPIO21** | I2C SDA | SSD1306（Wire.begin(21,38) 指定） |
| OLED_SCL | **GPIO38** | I2C SCL | SSD1306 |
| BTN_UP（菜单上） | **GPIO39** | 输入 | S3 有内部上拉（原 GPIO34 需外部上拉问题消失） |
| BTN_DOWN（菜单下） | **GPIO40** | 输入 | 同上 |
| PIN_IR_POWER（功率档） | **GPIO41** | 输出 | 940nm 远档大电流切换（高=远档） |
| IR_PWR_850_A（850nm 功率档 bit0） | **GPIO44** | 输出 | 850nm 选档 MOSFET Q_a 栅极（低=该电流支路断开） |
| IR_PWR_850_B（850nm 功率档 bit1） | **GPIO43** | 输出 | 850nm 选档 MOSFET Q_b 栅极（**可选**：若 43 被 UART0 占用则悬空，固件自动退化为 2 档） |
| BTN_OK（确认） | **GPIO42** | 输入 | 内部上拉（S3 GPIO0 保留给板载 BOOT，不用作按键） |
| BTN_CANCEL（取消） | **GPIO47** | 输入 | 内部上拉（GPIO43/44 留给 UART0 串口芯片，勿占用） |
| SX_DIO1 | — | **不接** | RadioLib 轮询模式（RadioLink.cpp 已轮询），留空 |
| （备用） | GPIO44 / GPIO47 | — | 预留扩展 |

## 3. 与原 ESP32 引脚对照

| 功能 | 原 ESP32 | 新 S3 | 变化要点 |
| --- | --- | --- | --- |
| IR_TX | 25 | 1 | |
| IR_TX_850 | 12 | 2 | S3 无 strapping 下拉需求 |
| IR_RX | 26 | 4 | |
| IR_RX_850 | 36 | 5 | S3 GPIO5 是普通双向 IO（不再是仅输入） |
| TRIGGER | 27 | 6 | |
| MOTOR | 32 | 7 | |
| LED_DATA | 15 | 8 | |
| I2S_BCLK/WS/DOUT | 14/4/33 | 9/18/11 | |
| SX 6 线 | 5/18/23/19/16/17 | 15/12/14/13/16/17 | |
| OLED SDA/SCL | 21/22 | 21/38 | |
| BTN_UP/DOWN | 34/35 | 39/40 | S3 可用内部上拉 |
| BTN_OK/CANCEL | 0/2 | 42/43 | 释放 GPIO0 给板载 BOOT |
| LIGHT_SENSE | 39 | 10 | ADC1_CH10（S3 的 ADC1 仅 GPIO1-10） |
| PIN_IR_POWER | 13 | 41 | |
| IR_PWR_850_A（新增） | — | 44 | 850nm 功率档 bit0 |
| IR_PWR_850_B（新增） | — | 43 | 850nm 功率档 bit1（可选，见 §4） |

## 4. 待核对项（用户/开发板侧）

1. **LCKFB-ESP32S3R8N8 DIP-40 排针引出核对**：确认 GPIO1/2/4–18、21、38–42、44、47 全部引出
   （排针丝印/原理图，立创开源广场：lckfb.com/project/detail/lckfb-esp32s3r8n8）。
2. **GPIO43/44 占用核对（关键）**：本表原注释"43/44 留给 UART0 串口芯片"，但 44 已分配
   给 850nm 功率档 bit0、43 给 bit1（可选）。**需确认开发板 UART0 是否实际引出到 43/44**：
   - 若未引出（USB 即串口）→ 两个引脚都可用，850nm 完整 4 档；
   - 若 43 被占用 → IR_PWR_850_B 悬空，固件退化为 2 档（config.h 将 B 改 0xFF）。
3. 若个别 GPIO 未引出，需在本表备用池（GPIO47 已用）或相邻引脚中替换，并同步
   修改 firmware/gun/src/config.h。
4. 板载 5V/3V3/GND 排针位置（载板供电走排针）。
5. 确认后：同步修改 `firmware/gun/src/config.h`（本表 GPIO 号）并更新
   docs/hardware-design.md 接线表。
=======
# main 板 ESP32-S3 引脚映射（gun-board.eprj2 / 板子 main）——定稿

> 状态：**已定稿（2026-09-04）**，依据 gun-board 主控页原理图 P1 导出的
> `Netlist_ESP32主控_2026-09-04.enet` 与 `SCH_ESP32主控_2026-09-04.pdf`。
> **丝印约定已验证**：U1 排针丝印 `Gxx` = GPIOxx（DIP-40 表与 Protoflow KiCad
> 镜像逐脚一致，见 §6）。
>
> 标注：✅ = 原理图已连线（图确认，勿改）；🆕 = 空脚建议分配
> （EasyEDA 里按此接线后回填 ✅；固件已按本表写入）。

## 1. 功能 → S3 GPIO 映射表（固件 config.h 同值）

| 功能 | DIP 脚 | S3 GPIO | 标注 | 说明（主控页网络） |
| --- | --- | --- | --- | --- |
| IR_TX（940 远距 OOK） | 14 | **13** | ✅ | $1N92 → 220Ω → Q4 栅（H2 排针接 940 LED） |
| IR_TX_850（近距 940/56k） | 27 | **47** | ✅ | '850nm' → 200Ω → Q1 栅 |
| IR_PWR_850_A | 29 | **39** | ✅ | '850nm-2' → Q2 栅 |
| IR_PWR_850_B | 28 | **38** | ✅ | '850nm-3' → Q3 栅 |
| IR_RX（38k XL-IRM 接收） | 4 | **14** | 🆕 | 空脚；XL-IRM-V838M3/TR（C51900936）。**2026-09-10 由 G04 迁到 G14**（G04 让给 SX_DIO1） |
| IR_RX_850（56k 预留） | 5 | **5** | 🆕 | 空脚；56k 暂缓 |
| PIN_IR_POWER（940 功率档） | — | **0xFF** | — | **取消**：G14 已让给 IR_RX；940 通道是 R447=47Ω 单档硬件，本就不需要（config 置 0xFF） |
| TRIGGER（扳机） | 6 | **6** | ✅ | G06 悬空 stub（主控页 $1N93），接微动开关 |
| MOTOR（水弹电机） | 8 | **7** | 🆕 | 空脚 |
| LED_DATA（WS2812） | 9 | **8** | 🆕 | 空脚 |
| I2S_BCLK / WS / DOUT | 9/12/11 所在脚 | **9 / 12 / 11** | 🆕 | 空脚（MAX98357A；WS 原 18 让给 OLED SCL） |
| OLED_SDA / SCL | 34 / 33 | **17 / 18** | ✅ | SDA=G17、SCL=G18 → OLED1 |
| LIGHT_SENSE（环境光 ADC） | 11 | **10** | 🆕 | 空脚；ADC1_CH10 |
| BTN_UP / DOWN / OK / CANCEL | 1/2/31/30 所在脚 | **1 / 2 / 21 / 40** | 🆕 | 空脚（内部上拉；GPIO0 保留板载 BOOT） |
| SX_NSS | 35 | **16** | ✅ | CS=G16 → U2.NSS |
| SX_SCLK | 38 | **42** | ✅ | SCK=G42 |
| SX_MOSI | 36 | **15** | ✅ | MOSI=G15 |
| SX_MISO | 37 | **41** | ✅ | MISO=G41 |
| SX_RST | 40 | **46** | ✅ | RaRES=G46（低有效） |
| SX_BUSY | 39 | **45** | ✅ | RaBUSY=G45 |
| SX_DIO1 | 4 | **4** | ✅ | **2026-09-10 已接线**：DIO1=G04 → SX1268 中断输出（RadioLib 第 2 参数）。
接上后阻塞 `transmit()`/`receive()` 恢复正常（~9.3ms，而非白等 46ms），中断回调/深睡唤醒可用 |
| 电源 | 17/24/20/21 | 3.3V/5V/GND | ✅ | +3.3V、+5V、GND×2 |

> ### ⚠️ 2026-09-10 改线记录（DIO1 → G04）
> 为了接 SX1268 的 DIO1（中断输出），做主控页/载板改线时做了三处调整：
>
> | 引脚 | 改前 | 改后 | 原因 |
> | --- | --- | --- | --- |
> | **G04** | IR_RX（38k 接收头） | **SX_DIO1** | DIO1 需要一个中断脚；G04 是 🆕 空脚，腾挪代价最小 |
> | **G14** | PIN_IR_POWER（940 功率档） | **IR_RX** | G14 的规划功能本就是"可选/不需要"（940 单档硬件），拿它接 IR_RX 不丢任何真功能 |
> | PIN_IR_POWER | G14 | **0xFF（取消）** | 940 通道 R447=47Ω 单档硬件，`LaserCodec` 对 0xFF 自动跳过 |
>
> 固件侧同步：`firmware/gun/src/config.h`、`firmware/gun-selftest/src/config.h`、
> `firmware/lora-gwtest/src/config.h` 已全部改为 `PIN_SX_DIO1 4`；
> 两个测试固件还用 DIO1 中断取收包到达时刻（±µs，替代轮询时刻）。
> **下一版 PCB 原理图请照此画线**（当前主控页只有 6 根线到 U2：NSS/SCK/MOSI/MISO/RST/BUSY）。

## 2. 已占用 GPIO 汇总（26 个）

`1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,45,46,47`

**仍然空闲可扩展**：G03、G48（GPIO3 为 strapping 建议避开；GPIO48=板载 LED 保留）。

> 2026-09-10 后：**G04 用于 SX_DIO1**（不再是 IR_RX），**G14 用于 IR_RX**（不再是功率档）
> —— 占用集合不变，只是其中两项换了功能。

## 3. S3 硬约束（与 §1 核对通过）

- GPIO26–37：Flash/Octal PSRAM 占用 → 全表未用 ✓
- GPIO19/20：USB → 未用 ✓；GPIO0：板载 BOOT → 未用 ✓
- ⚠️ GPIO45/46（SX BUSY/RST）：strapping 脚，主控页确认使用（立创板排针引出、上电时序无碍）

## 4. 待办

1. EasyEDA 主控页按 🆕 项接线（IR_RX=G04、IR_RX_850=G05、MOTOR=G07、LED=G08、
   I2S=G09/G11/G12、LIGHT=G10、IR_POWER=G14、BTN=G01/G02/G21/G40），完成后回填 ✅ 并重导 netlist 复核；
2. 若 940 通道为单档硬件（主控页 Q4/R447=47Ω 单档），把 `PIN_IR_POWER` 置 0xFF；
3. 其余板卡（头盔/校准靶/网关）仍为经典 ESP32 引脚方案，不受本表影响。

## 5. 备注

- 主控页 IR 驱动网络沿用历史命名 `850nm/-2/-3`（对应 Q1 OOK、Q2/Q3 功率档），
  波长已统一 940nm，网络名暂不改（PCB 侧无需动）。
- 原理图当前仅 P1（ESP32主控 页 1/1）；XL-IRM 接收头等后续功能页画入后更新本表。

## 6. 参考资料

- netlist：`PCB/gun-board/Netlist_ESP32主控_2026-09-04.enet`
- 原理图：`PCB/gun-board/SCH_ESP32主控_2026-09-04.pdf`（页 1/1）
- DIP-40 引脚表交叉验证：[Protoflow LCKFB-ESP32S3R8N8 KiCad](https://www.protoflow.ai/library/parts/C20626143)
  、[立创商城 LCKFB-ESP32S3R8N8](https://item.szlcsc.com/22034693.html)（C20626143）





















>>>>>>> .theirs
