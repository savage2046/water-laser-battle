# calibration-target 嘉立创EDA 工程解析报告（接线依据）

> 用途：校准靶板——**15 路 940nm(38k) 接收 + 每格 RGB 指示**（收到=红 / 未收到=绿）。
> 数据来源：`PCB/calibration-target/calibration-target_backup/calibration-target_2026-09-04-18-06.epro2`
> （`.eprj2` 文档内容为加密存储，无法直接读取；已与用户确认 18:06 之后原理图无改动，本报告
> 即当前版接线依据）。由 `tools/` 思路脚本解析 `.epru` 文档流 + PCB 网表（PAD_NET）自动整理。
> 主控 = **LCKFB-ESP32S3R8N8 立创开发板**（C20626143，DIP-40，丝印 `Gxx` = `GPIOxx`）。

## 1. 板卡结构（18:06 快照）

| 项 | 内容 |
| --- | --- |
| 原理图 | SCH ebe873c066336bd3，单页 SCH_PAGE ccb72a073721fdbc（84 元件对象） |
| PCB | 632a9d18a5855f23：37 元件、41 网络、54 过孔、1 覆铜区 |
| 器件种类 | IRM×15、WS2812C-2020-V6×15、ESP32-S3×1、74HCT1G125×1、电容×5 |

## 2. BOM（原理图实放器件）

| 位号 | 器件 | LCSC | 说明 |
| --- | --- | --- | --- |
| U32 | LCKFB-ESP32S3R8N8 开发板 | C20626143 | 主控（DIP-40） |
| U1..U10, U21..U25 | XL-IRM-V838M3/TR（38k/940nm IRM） | C51900936 | 15× 接收（4 脚：1=GND 2=VS 3=OUT 4=GND） |
| U11..U20, U41..U45 | WS2812C-2020-V6 | C55109522 | 15× RGB 指示（1=DO 2=GND 3=DI 4=VDD） |
| U33 | 74HCT1G125GV,125 | C12502 | 灯串 3.3V→5V 电平转换（1=OE# 2=A 3=GND 4=Y 5=VCC） |
| C1 | FCC0603B104K500CT 100nF | C5137636 | +3.3V 去耦 |
| C2..C5 | CL10A226MQ8NRNC 22µF | C59461 | +5V 储能 |

> 器件库内另有 WS2812(5050)/AO3400A/220Ω 等未在板使用（上一版遗留，未放置）。

## 3. ESP32-S3（U32）引脚映射

原理图符号 40 脚（DIP 脚号=符号脚号），引脚名 `Gxx`=GPIOxx（与 gun-board 主控页同款
LCKFB 板验证一致：DIP36=G15、DIP37=G41、DIP38=G42 等）。按 PCB PAD_NET 实测：

| DIP脚 | 引脚名 | 网络 | 用途 |
| --- | --- | --- | --- |
| 1 | G01 | IRM1 | IRM1.OUT |
| 2 | G02 | IRM2 | IRM2.OUT |
| 38 | G42 | IRM3 | IRM3.OUT（GPIO3 为 strapping → 原理图特意改走 G42） |
| 4 | G04 | IRM4 | IRM4.OUT |
| 5 | G05 | IRM5 | IRM5.OUT |
| 6 | G06 | IRM6 | IRM6.OUT |
| 8 | G07 | IRM7 | IRM7.OUT |
| 9 | G08 | IRM8 | IRM8.OUT |
| 10 | G09 | IRM9 | IRM9.OUT |
| 11 | G10 | IRM10 | IRM10.OUT |
| 12 | G11 | IRM11 | IRM11.OUT |
| 13 | G12 | IRM12 | IRM12.OUT |
| 14 | G13 | IRM13 | IRM13.OUT |
| 15 | G14 | IRM14 | IRM14.OUT |
| 36 | G15 | IRM15 | IRM15.OUT |
| 37 | G41 | $1N97 | → U33.A（灯串数据） |
| 17/18 | 3.3V | +3.3V | IRM VS 供电 |
| 21 | GND | GND | 地 |
| 24 | 5V | +5V | 灯串/74HCT 供电 |
| 其余 | — | 空 | 3/7/16/19/20/22/23/25..35/39/40 未接 |

**IRM 编号 ↔ GPIO 一览（固件 config.h 同值）**

```
IRM1→GPIO1  IRM2→GPIO2  IRM3→GPIO42  IRM4→GPIO4   IRM5→GPIO5
IRM6→GPIO6  IRM7→GPIO7  IRM8→GPIO8   IRM9→GPIO9   IRM10→GPIO10
IRM11→GPIO11 IRM12→GPIO12 IRM13→GPIO13 IRM14→GPIO14 IRM15→GPIO15
```

## 4. 关键网络（PCB 网表）

- **IRM1..15**：`IRMx` 网络 = Ux.OUT ↔ U32 对应 GPIO（见 §3）；IRM 供电 VS=+3.3V、OUT 低有效。
- **WS2812C 灯串**：`GPIO41($1N97)` → U33.A → U33.Y(`WS2812C` 网络) → U11.DI；
  `U11.DO($1N42)→U12.DI → … → U20.DO($1N88)→U41.DI → … → U45.DO($1N80)` 悬空（链尾）。
  链序：U11,U12,U13,U14,U15,U16,U17,U18,U19,U20,U41,U42,U43,U44,U45。
  每颗 VDD=+5V、GND=GND。
- **74HCT1G125（U33）**：1=OE#→GND（常通）、2=A←G41、3=GND、4=Y→灯串、5=VCC=+5V。
- **电源**：+5V（C2..C5 22µF）、+3.3V（C1 100nF）、GND。

### LED↔IRM 同格关系（PCB 坐标邻接实测）

```
列1(IRM1-5↓)          列2(IRM10-6↑)          列3(IRM11-15↓)
IRM1  LED0 (U11)   |  IRM10 LED9 (U20)   |  IRM11 LED10(U41)
IRM2  LED1 (U12)   |  IRM9  LED8 (U19)   |  IRM12 LED11(U42)
IRM3  LED2 (U13)   |  IRM8  LED7 (U18)   |  IRM13 LED12(U43)
IRM4  LED3 (U14)   |  IRM7  LED6 (U17)   |  IRM14 LED13(U44)
IRM5  LED4 (U15)   |  IRM6  LED5 (U16)   |  IRM15 LED14(U45)
```

链序 LED 索引 k ↔ IRM(k+1) 同格 → 固件按索引一一对应即可。

## 5. 遗留问题（建议 EDA 内核对，均不影响固件引脚表）

1. **IRM 电源/地脚网络不统一**：U1..U5 的 4 脚封装为 1=GND/2=+3.3V/3=OUT/4=GND；
   但 U6..U10 解析为 1&2=+3.3V、4=`$1N60`；U21..U25 为 1=`$1N77`、4=`$1N71`（2=+3.3V）。
   若这些 `$1N60/$1N71/$1N77` 未并入 GND 覆铜，则 U6..U10、U21..U25 的地会悬空，
   接收头无法工作——请在嘉立创EDA里打开 DRC/网络检查确认并补接 GND（可能只是解析时
   未合并覆铜导致，需以 EDA 内实际网络为准）。
2. **灯串电流**：15 颗 5V 全亮（绿）约 150~300mA，由 U32 的 5V 排针（USB 供）出；
   `FASTLED_BRIGHTNESS` 建议 ≤150，必要时 5V 外供。
3. **WS2812C-2020-V6 协议**：按 WS2812 系 800kHz 单线驱动（FastLED NEOPIXEL）；若实测
   颜色错乱/不亮，确认是否为需 `WS2811_400` 的特殊版本。
4. U32 未用脚（DIP 3/7/16/19/20/22/23/25~35/39/40）留空，符合“避开 strapping/USB/PSRAM”约束。
5. C2..C5(22µF) 均已接 +5V；+3.3V 仅 C1(100nF)（IRM 15 路靠开发板自身 3.3V LDO 余量，可接受）。

## 6. 固件对应

`firmware/calibration-target`（ESP32-S3 / PlatformIO）：
GPIO 表同 §3，灯串 GPIO41；普通格"收到→红 / 未收到→绿"。
**U8（IRM8 = 格7 = GPIO8）额外完整解码**（复用枪端 NEC 风格 40bit 帧，`FrameRx` 通道0）：
解码成功且校验通过 → U8 格蓝闪 3 次 + 串口打印全部解码信息（raw 5 字节 + P/W/T/S）。
详见该目录 README 与 `src/config.h`。
