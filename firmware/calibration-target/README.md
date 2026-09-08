# calibration-target 校准靶板固件（ESP32-S3）

按新版板卡原理图（`PCB/calibration-target/calibration-target.eprj2`，2026-09-04 保存）
新建的独立固件。板卡 = **15 路 940nm(38k) 接收 + 15 颗 WS2812C-2020 RGB 指示**：

- 某格 IRM 收到 **940nm(38k) 载波** → 该格 LED **红**
- 未收到的格 → LED **绿**
- **U8（IRM8）额外完整解码子弹帧**：解出完整子弹编码 → 该格**蓝闪 3 次**，串口打印全部解码信息
- **开机扫灯自检**：上电逐颗亮起 15 颗灯，核对位置对应

> 与旧 `firmware/target`（v3：OLED+中心格解码+WS2812B）不同：本固件严格按新板
> （WS2812C-2020-V6 灯串 GPIO41、IRM3→GPIO42、去掉 OLED/蜂鸣器）编写。

## 1. 接线（按原理图，模块丝印 Gxx = GPIOxx）

### 1.1 电源

| 网络 | 接到 | 说明 |
| --- | --- | --- |
| +3.3V | U32.17/18（3.3V）、U1~U10/U21~U25 全部 IRM 的 VS | 15× XL-IRM 供电（C1 100nF 就近去耦） |
| +5V | U32.24（5V）、15× WS2812C 的 VDD、U33.5(VCC=74HCT1G125) | 灯串/电平转换供电（C2~C5 22µF 就近储能） |
| GND | U32.21、所有器件 GND 共地 | 与灯串 5V 地同网络 |

### 1.2 15 路接收（IRM1..IRM15 → 对应 GPIO）

| IRM | 网络 | S3 GPIO | 模块 DIP 脚 | 备注 |
| --- | --- | --- | --- | --- |
| IRM1 | IRM1 | **GPIO1** | 1 | U1（焊位 U1..U10/U21..U25 见解析报告） |
| IRM2 | IRM2 | **GPIO2** | 2 | |
| IRM3 | IRM3 | **GPIO42** | 38 | GPIO3 为 strapping，原理图改走 GPIO42 |
| IRM4 | IRM4 | **GPIO4** | 4 | |
| IRM5 | IRM5 | **GPIO5** | 5 | |
| IRM6 | IRM6 | **GPIO6** | 6 | |
| IRM7 | IRM7 | **GPIO7** | 8 | |
| IRM8 | IRM8 | **GPIO8** | 9 | |
| IRM9 | IRM9 | **GPIO9** | 10 | |
| IRM10 | IRM10 | **GPIO10** | 11 | |
| IRM11 | IRM11 | **GPIO11** | 12 | |
| IRM12 | IRM12 | **GPIO12** | 13 | |
| IRM13 | IRM13 | **GPIO13** | 14 | |
| IRM14 | IRM14 | **GPIO14** | 15 | |
| IRM15 | IRM15 | **GPIO15** | 36 | |

每个 IRM：VS=+3.3V、OUT=对应 GPIO（内部上拉、TTL 低有效，收到 38k 载波时 OUT 拉低）、GND。
（IRM 输出是推挽式，板上未配外部上拉，固件 `INPUT_PULLUP` 仅为兜底。）

### 1.3 LED 灯串（WS2812C-2020-V6 ×15，800kHz 单线）

```
GPIO41(U32.DIP37=G41) → U33 74HCT1G125.A
74HCT1G125：VCC=+5V(脚5)，GND(脚1/3)，OE#=GND(脚1，常通)，Y(脚4) → 灯串 DIN
灯串链序：U11 → U12 → … → U20 → U41 → U42 → … → U45（共 15 颗）
```

- **链序 LED 索引 0..14 ↔ IRM 1..15（同格一一对应）**，位置关系：

```
列1(IRM1-5↓)        列2(IRM10-6↑)       列3(IRM11-15↓)
IRM1  LED0(U11)  |  IRM10 LED9(U20)   |  IRM11 LED10(U41)
IRM2  LED1(U12)  |  IRM9  LED8(U19)   |  IRM12 LED11(U42)
IRM3  LED2(U13)  |  IRM8  LED7(U18)   |  IRM13 LED12(U43)
IRM4  LED3(U14)  |  IRM7  LED6(U17)   |  IRM14 LED13(U44)
IRM5  LED4(U15)  |  IRM6  LED5(U16)   |  IRM15 LED14(U45)
```

灯串数据用 3.3V GPIO 驱动时电平不够（5V 灯 Vih≈3.5V），板上已按上图用
**74HCT1G125（5V 供电）** 抬到 5V——固件侧无需额外电平转换。

## 2. 行为与参数（`src/config.h`）

- **14 格（U8 除外）**：IRM 收到 940nm(38k) → 红（保持 `LED_HIT_HOLD_MS`）；未收到 → 绿
- **U8（IRM8 = 格7 = GPIO8）**：除同样"有 38k 就红"外，额外做**完整 40bit 帧解码**
  （与枪端 NEC 风格帧一致，`FrameRx`）：
  - 解出完整子弹编码且校验通过 → U8 格 **蓝闪 3 次**（亮 150ms / 灭 150ms）
  - 同时**串口打印全部解码信息**（见下）
  - U8 引脚被解码器独占（CHANGE 中断），不再走普通"突发测宽"通道

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `LED_IDLE_COLOR` | `CRGB(0,90,0)` 绿 | 未接收显示色 |
| `LED_HIT_COLOR` | `CRGB(255,0,0)` 红 | 收到 940nm 显示色 |
| `LED_DECODE_COLOR` | `CRGB(0,0,255)` 蓝 | U8 解码成功闪烁色 |
| `DECODE_FLASH_TIMES/ON_MS/OFF_MS` | 3 / 150 / 150 | 蓝闪次数与亮灭时长 |
| `DECODE_CELL_IDX` / `IRM_GPIO_DECODE` | 7 / GPIO8 | U8 解码格（格号=IRM号-1） |
| `SELFTEST_ENABLE` | 1 | 开机逐颗扫灯自检（核对灯位） |
| `SELFTEST_STEP_MS` / `SELFTEST_COLOR` | 300 / 白 | 每颗点亮时长与颜色 |
| `IRM_MIN_BURST_US` | 150 | 有效载波突发最短时长（µs），滤噪声（38k 单周期≈26µs） |
| `LED_HIT_HOLD_MS` | 600 | 最后一次信号后 LED 保持红的时间；连续照射期间持续为红 |
| `LED_REFRESH_MS` | 25 | 灯串刷新周期 |
| `FASTLED_BRIGHTNESS` | 110 | 全局亮度 0~255（15 颗 5V，USB 供电请勿开满） |

**开机扫灯自检**：上电后灯串按链序 LED0→LED14（U11→U12→…→U45）逐颗点亮约 300ms，
用于核对"哪颗灯对应哪个位置"（LED 索引 k ↔ IRM(k+1) 同格，串口同步打印
`[selftest] LEDk ON: Uxx = IRMn cell (GPIOxx)`）；扫完回到全绿待机。

检测逻辑（普通格）：IRM OUT 挂 CHANGE 中断，量测低电平持续时长 ≥ `IRM_MIN_BURST_US`
记为一次"收到"，对应 LED 变红并保持 `LED_HIT_HOLD_MS`；无信号格恒绿。
连续 38k 照射（长按）由主循环维持为红。
U8 解码：`FrameRx` 采样边沿 → 40bit 帧 → `chk == ~(b0^b1^b2^b3)` 校验通过即成功。

### 串口输出（115200，原生 USB CDC）

解码成功后每帧一行（完整信息，含原始 5 字节与字段拆解；同枪重复帧仅闪一次但照常打印）：

```
[decode] ch0 t=123456ms raw=AABBCCDDEE chkOK P=0(0000) W=0 T=0 S=1
```

| 段 | 含义 |
| --- | --- |
| `ch0` | 解码通道 0=38k |
| `raw=…` | 原始 40bit 帧 5 字节（b0 b1 b2 b3 chk，按枪端编码） |
| `P=` | playerId（16bit，括号内十六进制） |
| `W=` | weaponId |
| `T=` | team |
| `S=` | shotSeq（子弹编码/序号） |

其余调试输出（开机 banner、命中掩码 `[hit] mask=0x…`）由 `SERIAL_DEBUG` 开关控制，
`[decode]` 行始终打印。

## 3. 编译烧录

```bash
cd firmware/calibration-target
pio run -t upload      # 默认烧到 USB 串口 /dev/ttyACM0(COMx)，原生 USB CDC
pio device monitor     # 115200，可看命中掩码 [hit] mask=0xXXXX
```

- 板卡环境 `esp32-s3-devkitc-1` + Arduino；依赖 FastLED（自动安装）。
- 换用别的 ESP32-S3 板时只需改 `src/config.h` 各 `IRM_GPIO_*` 与 `PIN_LED_DATA`。

## 4. BOM（原理图器件）

| 位号 | 器件 | LCSC | 说明 |
| --- | --- | --- | --- |
| U32 | LCKFB-ESP32S3R8N8 开发板 | C20626143 | 主控 |
| U1..U10,U21..U25 | XL-IRM-V838M3/TR | C51900936 | 15× 38k/940nm 接收头 |
| U11..U20,U41..U45 | WS2812C-2020-V6 | C55109522 | 15× RGB 指示 |
| U33 | 74HCT1G125GV,125 | C12502 | 灯串电平转换 |
| C1 | 100nF 0603 | C5137636 | +3.3V 去耦 |
| C2..C5 | 22µF 0603 | C59461 | +5V 储能 |

> 板卡更多细节（网表/几何布局/遗留疑问）见
> `PCB/calibration-target/calibration-target-解析报告.md`。
