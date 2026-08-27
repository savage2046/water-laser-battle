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

## 4. 待核对项（用户/开发板侧）

1. **LCKFB-ESP32S3R8N8 DIP-40 排针引出核对**：确认 GPIO1/2/4–18、21、38–42、47 全部引出
   （排针丝印/原理图，立创开源广场：lckfb.com/project/detail/lckfb-esp32s3r8n8）。
2. 若个别 GPIO 未引出，需在本表备用池（GPIO44/47）或相邻引脚中替换。
3. 板载 5V/3V3/GND 排针位置（载板供电走排针）。
4. 确认后：同步修改 `firmware/gun/src/config.h`（本表 GPIO 号）并更新
   docs/hardware-design.md 接线表。
