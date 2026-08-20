# triger-sensor-current-test 嘉立创EDA 项目解析报告

> ⚠️ **板卡性质：实验/测试主板** —— 用于**电流变化前期研究**（扳机/连发电流波形采集、阈值校准、INA226 性能验证），**后期实际 PCB 将重新设计**，本板不作为量产硬件依据。

> 由 `tools/` 下的解析脚本自动生成（读取嘉立创EDA专业版 `.epru` 文档流）。
> 数据来源（已修正版）：`PCB/triger-sensor-current-test_fixed/triger-sensor-current-test_2026-08-19-18-40.fixed.epro2`；
> 修正前原版备份：`PCB/backup_before_fix/triger-sensor-current-test_2026-08-19-18-40.orig.epro2`

## 1. BOM（原理图已放置元件）

| 位号 | 值 | 器件 | 厂家 | 料号(LCSC) | 封装 | 等级 |
| --- | --- | --- | --- | --- | --- | --- |
| C1 | 10uF | CL31A106KBHNNNE | SAMSUNG(三星) | C13585 | 1206 | Basic Part |
| C2 | 10uF | CL31A106KBHNNNE | SAMSUNG(三星) | C13585 | 1206 | Basic Part |
| CN1 | - | XT30PW-M20.G.Y | AMASS(艾迈斯) | C30170175 | - | Extended Part |
| CN2 | - | XT30PW-F20.G.Y | AMASS(艾迈斯) | C2913282 | - | Extended Part |
| R1 | 10mΩ | RE2512F3R010 | JIERR(捷而瑞) | C46961753 | 2512 | Extended Part |
| R2 | 2.2kΩ | 0603WAF2201T5E | UNI-ROYAL(厚声) | C4190 | 0603 | Basic Part |
| R3 | 2.2kΩ | 0603WAF2201T5E | UNI-ROYAL(厚声) | C4190 | 0603 | Basic Part |
| U1 | - | LCKFB-ESP32S3R8N8 | 立创开发板 | C20626143 | - | Extended Part |
| U2 | - | AMS1117-5.0_C6187 | Advanced Monolithic Systems | C6187 | SOT-223 | Basic Part |
| U3 | - | INA226AIDGSR | TI(德州仪器) | C49851 | VSSOP-10 | Extended Part |

共 **10** 个实体元件；另外还有 **14** 个网络标识/图纸符号（GND、+5V、+3.3V、VCC、VCC5、图纸边框）。

## 2. 器件库中已存在但**未放置**的器件（后续扩展用）

| 器件 | 料号(LCSC) | 封装 |
| --- | --- | --- |
| 0603WAF1002T5E | C25804 | 0603 |
| 0603WAF1003T5E | C25803 | 0603 |
| 0603WAF2202T5E | C31850 | 0603 |
| CC0603KRX7R9BB104 | C14663 | 0603 |
| CH340C | C84681 | SOP-16 |
| CL10B102KB8NNNC | C1588 | 0603 |
| CL21A226MAQNNNE | C45783 | 0805 |
| ESP32-WROOM-32 | C95209 | - |
| HR1117V-5.0 | C54560849 | SOT-89 |
| HoLLR2512-3W-10mR-1% | C2994644 | 2512 |
| MP1584EN-LF-P | C7304223 | SOIC-8-EP |
| XT30U-F | C99102 | - |
| 功率电感(贴片),CKST0502-10uH/M,10uH,±20%,3A,封装5.7mm*5.1mm*2.0mm,岑科,RoHS | C9900015114 | 功率电感(贴片),CKST0502-10uH/M,10uH,±20%,3A,封装5.7mm*5.1mm*2.0mm,岑科,RoHS |
| 立创·ESP32S3开发板 | - | - |
| 贴片USB-3.1插座 Type-C母座 16P | C9900000123 | SMD-16P |

## 3. 网表

- **+3.3V**：+3.3V.1(Pin1), R2.1(1), R3.2(2), U1.17(3.3V), U1.18(3.3V), U1.19(3.3V), U3.6(VS+)
- **+5V**：+5V.1(Pin1), C2.2(2), U1.22(5V), U1.23(5V), U1.24(5V), U2.2(VOUT), U2.4(VOUT)
- **GND**：C1.2(2), C2.1(1), CN1.2(-), CN1.3(G), CN1.4(G), CN2.2(2), CN2.3(3), CN2.4(4), GND.1(Pin1), U2.1(GND), U3.1(A1), U3.2(A0), U3.7(GND)
- **VCC**：C1.1(1), CN2.1(1), R1.1(1), U2.3(VIN), U3.10(VIN+), VCC.1(Pin1)
- **VCC5**：VCC5.1(Pin1)
- **NET1**：CN1.1(+), R1.2(2), U3.8(VBUS), U3.9(VIN-)
- **NET10**：U1.6(G06)
- **NET11**：U1.7(GND)
- **NET12**：U1.8(G07)
- **NET13**：U1.9(G08)
- **NET14**：U1.10(G09)
- **NET15**：U1.11(G10)
- **NET16**：U1.12(G11)
- **NET17**：U1.13(G12)
- **NET18**：U1.14(G13)
- **NET19**：U1.15(G14)
- **NET2**：U3.3(Alert)
- **NET20**：U1.16(RST)
- **NET21**：U1.20(GND)
- **NET22**：U1.40(G46)
- **NET23**：U1.39(G45)
- **NET24**：U1.38(G42)
- **NET25**：U1.37(G41)
- **NET26**：U1.36(G15)
- **NET27**：U1.35(G16)
- **NET28**：U1.32(GND)
- **NET29**：U1.31(G21)
- **NET3**：R2.2(2), U1.34(G17), U3.4(SDA)
- **NET30**：U1.30(G40)
- **NET31**：U1.29(G39)
- **NET32**：U1.28(G38)
- **NET33**：U1.27(G47)
- **NET34**：U1.26(G48)
- **NET35**：U1.25(BOOT)
- **NET36**：U1.21(GND)
- **NET4**：R3.1(1), U1.33(G18), U3.5(SCL)
- **NET5**：U1.1(G01)
- **NET6**：U1.2(G02)
- **NET7**：U1.3(G03)
- **NET8**：U1.4(G04)
- **NET9**：U1.5(G05)

## 4. 固件引脚/参数核对（firmware/trigger-board/src/config.h）

| 固件项 | 固件值 | 原理图实际 | 状态 |
| --- | --- | --- | --- |
| I2C SDA | GPIO17 | NET3 → U1.34 (G17) | ✅ 一致 |
| I2C SCL | GPIO18 | NET4 → U1.33 (G18) | ✅ 一致 |
| INA226 地址 | 0x40 | A0=GND, A1=GND（接地→0x40） | ✅ 一致 |
| 分流电阻 | SHUNT_MOHM=10mΩ | R1 = 10mΩ (RE2512F3R010) | ✅ 一致 |
| MOSFET 断电 | PIN_MOSFET=15 | 原理图无 MOSFET 电路 | ⚠️ 缺失 |
| 状态 LED | PIN_LED=2 | 原理图无 LED 电路 | ⚠️ 缺失 |
| ACS712 | PIN_ACS712=34 | 未放置（固件已默认 INA226 方案） | ℹ️ 未用 |

## 5. 修正记录（2026 本次会话）

针对上一版解析报告中的不一致，已直接修改 `.epru` 原理图文档流并重新打包为 `PCB/triger-sensor-current-test_fixed/triger-sensor-current-test_2026-08-19-18-40.fixed.epro2`：

| # | 修正项 | 说明 |
| --- | --- | --- |
| 1 | CN1（XT30 公头）接线 | 1(+)=电机回路高端（R1 低端，与 INA226 VIN-/VBUS 同网）；2(-)/3(G)/4(G)=GND —— 电流检测回路闭合，电机电流流经 R1 |
| 2 | CN2（XT30 母头）G 引脚 | 3(G)/4(G) 由悬空改为接 GND |
| 3 | U1 ESP32-S3 5V 引脚 | 22/23 并联到 +5V（与 pin24 一致） |
| 4 | U1 ESP32-S3 3.3V 引脚 | 17/18 并联到 +3.3V（与 pin19 一致） |
| 5 | 2.2kΩ 上拉电阻位号 | 补 R2（SDA 上拉）/ R3（SCL 上拉） |
| 6 | 固件 I2C 引脚 | config.h `PIN_SDA 21→17`、`PIN_SCL 22→18`（原理图 G17/G18；ESP32-S3 开发板未引出 G22） |
| 7 | 固件分流阻值 | config.h `SHUNT_MOHM 2→10`（与 R1=10mΩ 一致） |
| 8 | 文档同步 | docs/trigger-board.md、roadmap.md 更新为 17/18 + 10mΩ |

## 6. 剩余问题（需在嘉立创EDA 内处理）

1. **U1（ESP32-S3 开发板）GND 引脚全部悬空**（pin7/20/21/32）：只接了 5V/3.3V 没有地回路，板子无法工作。此布局下引脚被 I2C 竖线与相邻引脚包围，无法安全自动布线，需在 EDA 里移动 R2/R3 或 I2C 走线后补接（任接一个 GND 引脚即可）。
2. **MOSFET 断电开关电路缺失**（固件 `PIN_MOSFET 15` 已定义）：需新增 N-MOS + 栅极电阻/下拉，串联在 CN1.2(-)→GND 回路中。
3. **状态 LED 缺失**（固件 `PIN_LED 2` 已定义）。
4. **AMS1117-5.0 输入/输出均为 5V 级网络**：若 XT30 输入是 5V 则这级 LDO 无意义；若输入是 7.4V/12V 电池（推荐），建议把 VCC 网络标签改成 VBAT/+B 避免误解。
5. **INA226 Alert 未连接**：轮询模式可用；若要扳机零延迟中断，接到 ESP32-S3 空闲 GPIO（如 G19/G20 未引出则用 G15/G16）。
6. **VCC5 网络标签悬空**（(780,-244) 处）：无元件连接，可删除或接线。

## 7. 板卡状态

- 原理图（SCH_PAGE P1）：**已修正**（新增 8 条导线 + 2 个位号）。
- PCB（PCB1, ceb442a27ffb448c）：**空板**——.epru 中 PCB 文档只有 META，无元件/走线/覆铜，待原理图确认后在 EDA 内转 PCB。
- 项目结构：2 块板（电流检测 / INA226），1 个原理图，1 个 PCB。
