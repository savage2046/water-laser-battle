# triger-sensor 嘉立创EDA 项目解析报告（正式板）

> ✅ **正式板**：无改装水弹枪联动检测板——电流检测扳机/连发 + MOSFET 物理断电
> + ESP-NOW 连枪端主控。由实验板（triger-sensor-current-test）迭代重画，
> 自带 USB-C（原生 USB 编程/供电）与独立电源（XL1509 5V + AMS1117 3.3V）。

> 数据来源（可读备份）：`PCB/triger-sensor_backup/triger-sensor_2026-08-31-11-37.epro2`（git HEAD 保留）；

> 工程文件：`PCB/triger-sensor.eprj2`（嘉立创EDA专业版，2026-09-01 保存，内容与上述备份一致）。

> 由 `tools/` 解析脚本读取 `.epru` 文档流自动生成。

## 1. 板卡结构

| 项 | 内容 |
| --- | --- |
| 板卡 | 电流采样触发（board 0949e4de03fd39c1） |
| 原理图 | INA226（schematic a30c1169d5365697，sheet P1） |
| PCB | 电流采样（a63391f5039f097a）——**已摆件/布线中**：26 元件、30 过孔、5 覆铜区、32 网络 |
| 块符号 | INA226（docType 17） |

## 2. BOM（原理图已放置元件，共 26 个实体元件）

| 位号 | 值 | 器件 | 厂家 | 料号(LCSC) | 封装 | 等级 |
| --- | --- | --- | --- | --- | --- | --- |
| C1 | 22uF | HGC0603R5226M100NTHJ | Chinocera(华瓷) | C7432770 | 0603 | Extended Part |
| C2 | 22uF | HGC0603R5226M100NTHJ | Chinocera(华瓷) | C7432770 | 0603 | Extended Part |
| C3 | 22uF | HGC0603R5226M100NTHJ | Chinocera(华瓷) | C7432770 | 0603 | Extended Part |
| C4 | 22uF | HGC0603R5226M100NTHJ | Chinocera(华瓷) | C7432770 | 0603 | Extended Part |
| C5 | 22uF | HGC0603R5226M100NTHJ | Chinocera(华瓷) | C7432770 | 0603 | Extended Part |
| CN1 | - | XT30PW-M20.G.Y | AMASS(艾迈斯) | C30170175 | - | Extended Part |
| CN2 | - | XT30PW-F20.G.Y | AMASS(艾迈斯) | C2913282 | - | Extended Part |
| D1 | - | SS34_C7420365 | R+O(宏嘉诚) | C7420365 | SMA(DO-214AC) | Extended Part |
| D2 | - | B5817WS_C7420329 | R+O(宏嘉诚) | C7420329 | SOD-323 | Extended Part |
| D3 | - | USBLC6-2SC6_C2827654 | TECH PUBLIC(台舟) | C2827654 | SOT-23-6 | Extended Part |
| L1 | 68uH | FNR6045S680MT | cjiang(长江微电) | C168091 | SMD,6x6mm | Extended Part |
| LED1 | - | NCD0805R1 | 国星光电 | C84256 | 0805 | Basic Part |
| Q1 | - | 20N03 | HL(富海微) | C19271469 | TO-252 | Extended Part |
| R1 | 2mΩ | JER2512F3R002 | JIERR(捷而瑞) | C46634447 | 2512 | Extended Part |
| R2 | 2.2kΩ | 0603WAF2201T5E | UNI-ROYAL(厚声) | C4190 | 0603 | Basic Part |
| R3 | 2.2kΩ | 0603WAF2201T5E | UNI-ROYAL(厚声) | C4190 | 0603 | Basic Part |
| R4 | 2.2kΩ | 0603WAF2201T5E | UNI-ROYAL(厚声) | C4190 | 0603 | Basic Part |
| R6 | 220Ω | 0603WAF2200T5E | UNI-ROYAL(厚声) | C22962 | 0603 | Basic Part |
| R9 | 5.1kΩ | FRC0603F5101TS | FOJAN(富捷) | C2907044 | 0603 | Extended Part |
| R10 | 5.1kΩ | FRC0603F5101TS | FOJAN(富捷) | C2907044 | 0603 | Extended Part |
| U1 | 2.4GHz | ESP32-C3-WROOM-02-N4 | ESPRESSIF(乐鑫) | C2934560 | SMD,20x18mm | Extended Part |
| U2 | - | XL1509-5.0E1 | XLSEMI(芯龙) | C61063 | SOIC-8 | Basic Part |
| U3 | - | INA226AIDGSR | TI(德州仪器) | C49851 | VSSOP-10 | Extended Part |
| U4 | - | UCC27517DBVR(UMW) | UMW(友台半导体) | C20623191 | SOT-23-5 | Extended Part |
| U7 | - | AMS1117-3.3_C6186 | Advanced Monolithic Systems | C6186 | SOT-223 | Basic Part |
| USB1 | - | TYPE-C 16PIN 2MD(073) | SHOU HAN(首韩) | C2765186 | SMD | Extended Part |

## 3. ESP32-C3-WROOM-02-N4（U1）引脚映射

| 引脚 | 名称 | 网络 | 用途 |
| --- | --- | --- | --- |
| 1 | +3.3V | +3.3V | 模块供电 |
| 2 | NET19 | NET19 | EN（R4 2.2kΩ 上拉到 3.3V） |
| 3 | SDA | SDA | I2C → INA226 SDA |
| 4 | SCL | SCL | I2C → INA226 SCL |
| 5 | NET18 | NET18 | IO6 预留 |
| 6 | NET17 | NET17 | IO7 预留 |
| 7 | NET16 | NET16 | IO8 预留 |
| 8 | NET15 | NET15 | IO9 预留 |
| 9 | GND | GND | 地 |
| 10 | NET14 | NET14 | IO10 预留 |
| 11 | NET13 | NET13 | RXD 预留 |
| 12 | NET12 | NET12 | TXD 预留 |
| 13 | D- | D- | USB D-（IO18，经 D3 USBLC6 到 USB1） |
| 14 | D+ | D+ | USB D+（IO19，经 D3 USBLC6 到 USB1） |
| 15 | OUT | OUT | IO3 → UCC27517.IN+（栅极驱动） |
| 16 | NET11 | NET11 | IO2 预留 |
| 17 | NET10 | NET10 | IO1 预留 |
| 18 | NET9 | NET9 | IO0 → R6(220Ω) → LED1 |
| 19 | NET8 | NET8 | EP 地焊盘 |

## 4. 电源链

```
XT30（CN1/CN2，电池/motor 回路外部闭合）→ +11.1V
  ├─ R1(2mΩ 2512 3W, JER2512F3R002) 串联电池回路 ← INA226 VIN+/VIN- 跨测
  ├─ U2 XL1509-5.0E1（11.1V→5V；L1 68uH + D2 B5817WS + C1~C3 22uF）
  └─ U4 UCC27517 VDD（栅极驱动供电，11.1V）
+5v：XL1509 输出 + USB-C VBUS（经 D3 USBLC6）→ U7 AMS1117-3.3 → +3.3V（C5 22uF）
+3.3V → U1.3V3 / U3.VS+ / R2/R3/R4 上拉
```

- **电流检测**：R1 = 2mΩ 3W 2512（JER2512F3R002，C46634447）；INA226 VIN+=+11.1V（R1.1 侧）、VIN-=NET2（R1.2 侧）、VBUS=+11.1V；A0/A1 接地 → 地址 0x40
- **断电执行**：U1.IO3 → U4.IN+ → U4.OUT → Q1.G(20N03)（直连，NET21）；HIGH=枪回路导通；断开即物理断电（防作弊硬保证）。注：R6(220Ω) 是 LED 限流（NET9/NET20），不在栅极回路
- **USB 编程/供电**：Type-C（USB1）D+/D- → D3（USBLC6-2SC6 ESD）→ IO19/IO18 原生 USB；CC1/CC2 经 R9/R10（5.1kΩ）下拉，免 CH340

## 5. 网表（几何解析）

- **+11.1V**：+11.1V.1(Pin1), R1.1(1), U2.1(VIN), U3.10(VIN+), U3.8(VBUS), U4.1(VDD)
- **+3.3V**：+3.3V.1(Pin1), C5.2(2), R2.1(1), R3.1(1), R4.1(1), U1.1(3V3), U3.6(VS+), U7.2(VOUT), U7.4(VOUT)
- **+5v**：+5v.1(Pin1), D3.5(VBUS), U7.3(VIN), USB1.A4B9(VBUS), USB1.B4A9(VBUS)
- **D+**：D+.1(BI), D3.1(IO1), D3.6(IO1), U1.14(IO19), USB1.A6(Dp1), USB1.B6(Dp2)
- **D-**：D-.1(BI), D3.3(IO2), D3.4(IO2), U1.13(IO18), USB1.A7(Dn1), USB1.B7(Dn2)
- **GND**：+5v.1(Pin1), C1.1(1), C1.2(2), C2.1(1), C2.2(2), C3.1(1), C3.2(2), C5.1(1), CN2.2(2), D2.2(K), D3.2(GND), GND.1(Pin1), L1.1(1), LED1.1(-), R10.1(1), R9.1(1), U1.9(GND), U2.3(FB), U2.5(GND), U2.6(GND), U2.7(GND), U2.8(GND), U3.1(A1), U3.2(A0), U3.7(GND), U4.2(GND), U4.4(IN-), U7.1(GND), USB1.A1B12(GND), USB1.B1A12(GND)
- **OUT**：OUT.1(IN), OUT.1(OUT), U1.15(IO3), U4.3(IN+)
- **SCL**：R3.2(2), SCL.1(IN), U1.4(IO5), U3.5(SCL)
- **SDA**：R2.2(2), SDA.1(IN), U1.3(IO4), U3.4(SDA)
- **NET1**：CN2.3(3)
- **NET10**：U1.17(IO1)
- **NET11**：U1.16(IO2)
- **NET12**：U1.12(TXD)
- **NET13**：U1.11(RXD)
- **NET14**：U1.10(IO10)
- **NET15**：U1.8(IO9)
- **NET16**：U1.7(IO8)
- **NET17**：U1.6(IO7)
- **NET18**：U1.5(IO6)
- **NET19**：R4.2(2), U1.2(EN)
- **NET2**：CN2.4(4), NET2.1(IN), NET2.1(OUT), Q1.2(D), R1.2(2), U3.9(VIN-)
- **NET20**：LED1.2(+), R6.1(1)
- **NET21**：Q1.1(G), U4.5(OUT)
- **NET22**：C4.1(1)
- **NET23**：C4.2(2)
- **NET24**：D1.1(A)
- **NET25**：D1.2(K)
- **NET26**：USB1.B8(SBU2)
- **NET27**：R9.2(2), USB1.A5(CC1)
- **NET28**：USB1.A8(SBU1)
- **NET29**：R10.2(2), USB1.B5(CC2)
- **NET3**：CN1.1(+), CN2.1(1), NET3.1(IN), NET3.1(OUT), Q1.3(S)
- **NET30**：USB1.14(EH)
- **NET31**：USB1.13(EH)
- **NET32**：D2.1(A), L1.2(2), U2.2(OUTPUT)
- **NET33**：U2.4(ON/OFF)
- **NET4**：CN1.2(-)
- **NET5**：CN1.3(G)
- **NET6**：CN1.4(G)
- **NET7**：U3.3(Alert)
- **NET8**：U1.19(EP)
- **NET9**：R6.2(2), U1.18(IO0)

> 几何网表对跨越走线/无焊点连接可能漏连（如 U2 周边 +5v 网络），关键连接以 §3/§4 与原理图为准。

## 6. 遗留问题（需在嘉立创EDA 内确认/处理）

1. **U2（XL1509）周边 +5v 网络几何解析不完整**：L1/D2/C1~C3 与 +5v 的连接在几何网表中表现为部分 NET 编号（NET32=U2.2+ L1.2 + D2.A；+5v 仅含 D3.5/U7.3/USB1 VBUS）——需确认 buck 输出经 L1 到 +5v 的连线与覆铜，建议 EDA 内补焊点/检查 DRC。
2. **D1（SS34）悬空**：已放置未接线（NET24/NET25 单脚）。若作为 XL1509 输入/输出保护或反接保护需接线；否则可删除或保留备用。
3. **C4 悬空**（NET22/NET23 单脚）：已放置未接线，确认是否应为某电源去耦。
4. **Q1 栅极直连**：网表显示 U4.OUT 直连 Q1.G（NET21 仅 2 脚），无栅极串联阻尼电阻。20N03 Qg≈10nC、UCC27517 4A 峰值驱动，直连可接受；若实测振铃明显可加 10~22Ω。
5. **USB1 SBU1/SBU2/EH 悬空**（NET26/NET28/NET30/NET31）：Type-C 16P 的 SBU 与外壳脚未接，确认不需要音频/附件支持即可。
6. **U1 大部分 GPIO 未接**（IO1/IO2/IO6~IO10/RXD/TXD/EP）：预留，符合设计。
7. **INA226 Alert 未接**（U3.3 = NET7 单脚）：轮询模式可用；若要扳机零延迟中断，接到 ESP32-C3 空闲 GPIO（如 IO1/IO2）。

## 7. 板卡状态

- 原理图（SCH_PAGE P1）：**正式板完整重画**（ESP32-C3 模组 + XL1509 + AMS1117-3.3 + USB-C + INA226 + UCC27517 + 20N03）。
- PCB（电流采样）：**已摆件/布线中**——26 元件、30 过孔、5 覆铜区、32 网络（.epru PCB 文档含 COMPONENT/VIA/POUR/NET/PAD_NET）。
- 固件对齐：`firmware/trigger-board/src/config.h` `PIN_SDA 4/PIN_SCL 5/PIN_MOSFET 3/PIN_LED 0`、`SHUNT_MOHM 2`；`platformio.ini` 板卡 `esp32-c3-devkitm-1`。
- 配套文档：[docs/trigger-board.md](../docs/trigger-board.md)、[docs/trigger-sensor-study.md](../docs/trigger-sensor-study.md)。
