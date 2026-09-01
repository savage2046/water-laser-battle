# PCB 目录说明

## 0. 枪端电源板（新板设计，`gun-board.eprj2`）

枪端独立电源板：**18350 电池 + USB-C 充电 + IP5306 集成电源管理
（充电/5V 升压/按键开关机/4LED 电量/保护），输出 5V 单轨**（3.3V 由
ESP32 DevKit 板载 LDO 提供）。嘉立创工程 `PCB/gun-board.eprj2`（板 18350-power）。
配套设计文档/绘图清单原在 `gun-power-board/` 目录，详见 git 历史。

## 1. 检测板（正式板，`triger-sensor.eprj2`）

> ✅ **当前主设计**：无改装水弹枪联动——电流检测扳机/连发 + MOSFET 物理断电
> + ESP-NOW 连枪端主控。由实验板（triger-sensor-current-test）迭代而来，
> **重画为独立正式板**：自带 USB-C（原生 USB 编程/供电）、XL1509 5V 降压、
> AMS1117-3.3，不再依赖 ESP32-S3 开发板。

### 板卡构成

| 项 | 内容 |
| --- | --- |
| 工程文件 | `PCB/triger-sensor.eprj2`（嘉立创EDA专业版，SQLite） |
| 板卡 | 电流采样触发（board） |
| 原理图 | INA226（schematic `INA226`，sheet P1） |
| PCB | 电流采样（**已摆件/布线中**：26 元件 + 30 过孔 + 5 覆铜区） |
| 数据来源 | 工程备份 `.epro2`（可读 .epru 流，git HEAD `PCB/triger-sensor_backup/`，最新 2026-08-31-11-37） |

> ⚠️ `.eprj2` 内部 `history_data` 为加密文档流，解析脚本读取的是配套
> `.epro2` 备份（git 历史中保留）。当前 `.eprj2` 更新于 2026-09-01，
> 内容与 2026-08-31 备份一致（365 条设计记录完全相同）。

### 主控与接口（ESP32-C3-WROOM-02-N4，U1）

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| I2C SDA / SCL | **IO4 / IO5** | INA226（0x40），2.2kΩ 上拉 R2/R3 |
| 栅极驱动 OUT | **IO3** | → UCC27517.IN+ → OUT → Q1(20N03) 栅极 |
| 状态 LED | **IO0** | → R6(220Ω) → LED1（0805 红光） |
| USB D- / D+ | IO18 / IO19 | 原生 USB，Type-C（USB1）+ USBLC6-2SC6 ESD（D3） |
| EN | EN | R4(2.2kΩ) 上拉到 3.3V |
| 未用 | IO1/IO2/IO6~IO10/RXD/TXD/EP | 预留 |

### 电源链

```
XT30(CN1/CN2 电池/motor 回路) → +11.1V
  ├─ R1(2mΩ 2512 3W, JER2512F3R002) 串联电池回路 ← INA226 VIN+/VIN- 跨测
  ├─ U2 XL1509-5.0E1（11.1V→5V，L1 68uH + D2 B5817WS + C1~C3 22uF）
  └─ U4 UCC27517 VDD（栅极驱动供电，11.1V）
+5v：XL1509 输出 + USB-C VBUS（经 D3 USBLC6）
  └─ U7 AMS1117-3.3 → +3.3V（C5 22uF）→ U1 3V3 / U3 VS+
```

- 电流检测：**R1 = JER2512F3R002（2mΩ 3W 2512，C46634447）**，INA226 地址 0x40
- 断电执行：IO3 → UCC27517（4A 栅极驱动）→ Q1 = **20N03**（N-MOS 20A/30V，TO-252），
  HIGH=枪回路导通；断开即物理断电
- 编程/供电：USB-C（TYPE-C 16PIN，C2765186）→ IO18/IO19 原生 USB，
  CC1/CC2 经 R9/R10（5.1kΩ）下拉；无需 CH340

### 关键物料（LCSC）

| 位号 | 器件 | 料号 | 封装 |
| --- | --- | --- | --- |
| U1 | ESP32-C3-WROOM-02-N4 | C2934560 | 模组 |
| U2 | XL1509-5.0E1（5V 2A buck） | C61063 | SOIC-8 |
| U3 | INA226AIDGSR | C49851 | VSSOP-10 |
| U4 | UCC27517DBVR(UMW) | C20623191 | SOT-23-5 |
| U7 | AMS1117-3.3 | C6186 | SOT-223 |
| Q1 | 20N03（N-MOS 20A/30V） | C19271469 | TO-252 |
| R1 | JER2512F3R002（2mΩ 3W ±1%） | C46634447 | 2512 |
| L1 | FNR6045S680MT（68uH） | C168091 | 6×6mm |
| D1 | SS34（肖特基 3A/40V） | C7420365 | SMA |
| D2 | B5817WS（肖特基） | C7420329 | SOD-323 |
| D3 | USBLC6-2SC6（USB ESD） | C2827654 | SOT-23-6 |
| USB1 | TYPE-C 16PIN 2MD(073) | C2765186 | SMD |
| CN1/CN2 | XT30PW-M20 / XT30PW-F20 | C30170175 / C2913282 | XT30 |
| LED1 | NCD0805R1（红光） | C84256 | 0805 |
| C1~C5 | 22uF 10V（HGC0603R5226M100NTHJ） | C7432770 | 0603 |
| R2/R3/R4 | 2.2kΩ（0603WAF2201T5E） | C4190 | 0603 |
| R6 | 220Ω（0603WAF2200T5E） | C22962 | 0603 |
| R9/R10 | 5.1kΩ（FRC0603F5101TS） | C2907044 | 0603 |

### 固件对齐（`firmware/trigger-board/src/config.h`）

- `PIN_SDA 4 / PIN_SCL 5`、`PIN_MOSFET 3`、`PIN_LED 0`（IO4/IO5/IO3/IO0）
- `SHUNT_MOHM 2`（R1 = 2mΩ）、`INA226_ADDR 0x40`
- `platformio.ini` 板卡 `esp32-c3-devkitm-1`

### 解析报告

详见 `triger-sensor-解析报告.md`（BOM、网表、GPIO 映射、电源链、遗留问题）。

## 2. 实验板（历史，`triger-sensor-current-test.eprj2`）

> ⚠️ **实验/测试主板**：用于**电流变化前期研究**（扳机/连发电流波形采集、阈值校准、
> INA226 性能验证）。已被 `triger-sensor.eprj2` 正式板取代，仅作历史参考。

- 原理图：INA226 + **ESP32-S3 开发板（LCKFB-ESP32S3R8N8）** + AMS1117-5.0
  + XT30 对插（CN1 接电机回路 / CN2 接电池）；I2C 走 G17/G18、MOSFET G10、LED G02
- PCB1：空板（未布局，被正式板替代）
- 修正记录：CN1/CN2 接地、U1 电源引脚并联、2.2kΩ 上拉补位号、
  固件 `PIN_SDA 17/PIN_SCL 18`、`SHUNT_MOHM 2`（2mΩ）、INA226 Config=0x06C7
- 备份：`PCB/triger-sensor-current-test_backup/*.epro2`（git 历史保留）
