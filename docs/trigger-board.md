# 检测板实现说明（无改装水弹枪联动）

> ✅ **本板（triger-sensor.eprj2）为正式板**：由实验板（triger-sensor-current-test）迭代重画，
> 独立完整设计——**ESP32-C3-WROOM-02-N4 模组 + USB-C 原生编程 + XL1509 5V 降压 +
> AMS1117-3.3 + INA226 + UCC27517 栅极驱动 + 20N03 断电开关**，不再依赖 ESP32-S3 开发板。
> 工程文件 `PCB/triger-sensor.eprj2`（嘉立创EDA专业版）；原理图 INA226（P1）、
> PCB 电流采样（已摆件/布线中）；解析报告见 `PCB/triger-sensor-解析报告.md`。

> 状态：**原理图/PCB 设计完成，固件引脚已对齐，待打样 + 实测**。配套方案研究见
> [trigger-sensor-study.md](trigger-sensor-study.md)；枪端集成点见本文 §6。

## 1. 工程与文件

| 文件 | 说明 |
| --- | --- |
| `PCB/triger-sensor.eprj2` | 正式板工程（嘉立创EDA专业版；板"电流采样触发"，原理图 INA226/P1，PCB"电流采样"） |
| `PCB/triger-sensor-解析报告.md` | 自动生成的解析报告：BOM、网表、GPIO 映射、电源链、遗留问题 |
| `PCB/triger-sensor_backup/*.epro2` | 工程备份（可读 .epru 流，git 历史保留，最新 2026-08-31） |
| `firmware/trigger-board/` | 检测板固件（ESP32-C3-WROOM-02-N4，Arduino） |
| `firmware/trigger-board/src/config.h` | 配对 MAC、引脚、电流阈值、帧类型 |
| `CurrentSense.h/.cpp` | INA226 采样 + 扳机/连发识别状态机 |
| `PowerSwitch.h` | MOSFET 断电开关（默认导通） |
| `EspNowLink.h/.cpp` | ESP-NOW 链路（上报开火/心跳/异常；接收断电/恢复） |
| `firmware/gun/src/GunEspNow.h/.cpp` | 枪端 ESP-NOW（开火事件源 + 断电/恢复指令 + 心跳监控） |

## 2. 硬件接线（正式板，triger-sensor.eprj2）

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| INA226 SDA / SCL | IO4 / IO5 | I2C（地址 0x40；R2/R3 = 2.2kΩ 上拉到 3.3V，见原理图） |
| INA226 分流 | 2mΩ 串联电池回路 | R1 = **JER2512F3R002（2mΩ 3W 2512，C46634447）**；INA226 VIN+=+11.1V、VIN-=NET2、VBUS=+11.1V |
| MOSFET 控制 | IO3 | **IO3 → U4.IN+（UCC27517DBVR 4A 栅极驱动）→ OUT → Q1.G（20N03）**，HIGH=枪回路导通 |
| 状态 LED | IO0 | IO0 → R6(220Ω) → LED1（0805 红光），每发闪灯 + 状态 |
| USB 编程/供电 | IO18/IO19（D-/D+） | Type-C（USB1）经 D3（USBLC6-2SC6 ESD）→ 原生 USB，免 CH340；CC1/CC2 经 R9/R10（5.1kΩ）下拉 |
| 供电 | 11.1V（3S 电池） | XT30（CN1/CN2）→ **U2 XL1509-5.0E1（11.1V→5V，L1 68uH + D2 B5817WS + C1~C3 22uF）** → +5v → **U7 AMS1117-3.3 → +3.3V**（C5 22uF）；USB-C 5V 亦可并入 +5v |

> 传感器：`CURRENT_SENSE_INA226`（推荐，正式板即此方案）。ACS712 已不在正式板 BOM，
> 固件保留编译开关作为替代方案（模拟输出接 ESP32 任意 ADC 引脚）。

## 3. 电流识别状态机

```
IDLE ──(电流>1A 持续 10ms 去抖)──► FIRING（第一发 onFirePulse）
FIRING ──(低→高上升沿，距上发≥20ms)──► onFirePulse（连发逐发）
FIRING ──(持续高电流 ≥2s)──► STALL（堵转：自动断电 + FRAME_FAULT 上报）
FIRING ──(低电流持续 ≥50ms)──► IDLE（发射结束）
STALL ──(电流回落或 5s 超时)──► IDLE
```

- **逐发同步音效**：每个脉冲上升沿发 `FRAME_FIRE` → 枪端立即触发开火逻辑
  （音效 + IR 发射 + F 帧），端到端 5-15ms
- **开机自校准**：`calibrateIdle(500ms)` 静置采样取空闲基线，扣除检测板自身耗电

## 4. ESP-NOW 帧协议（检测板 ↔ 枪端）

帧：`{ 0xA5, type, d0, d1 }`（4 字节），MAC 白名单配对，信道 1（可改）。

| 方向 | type | d0 | 说明 |
| --- | --- | --- | --- |
| 板→枪 | 0x01 FIRE | 序号 | 每发开火脉冲 |
| 板→枪 | 0x02 HB | 计数 | 1Hz 心跳（枪端 3s 超时判掉线） |
| 板→枪 | 0x03 FAULT | 0x01=堵转 | 异常（已自动断电保护） |
| 枪→板 | 0x10 POWER | 0=断 1=通 | 断电/恢复指令（ACK 发送） |

## 5. 断电执行

- 枪端 `gunPowerUpdate()`：`needPower = isAlive && ammo>0`，状态变化时发
  `FRAME_POWER` → 检测板 MOSFET 断开/导通
- 阵亡（本地命中/470M X 帧）或空弹（consumeShot 后 ammo=0）→ **物理断电，
  水弹无法发射（防作弊硬保证）**；重生（R）/装弹计时完成（ammo 恢复）→ 恢复
- 默认导通：未收到指令时水弹枪正常使用；检测板掉线时枪端心跳监控告警
- 堵转保护：持续大电流 ≥2s → 检测板自动断电（防烧电机/电池）

## 6. 枪端集成点（firmware/gun/src/main.cpp）

| 位置 | 改动 |
| --- | --- |
| `setup()` | `gunEspNow.begin(kBoardMac)`（`GUN_ESPNOW_ENABLE` 开关） |
| loop 扳机 | `bool trig = readTrigger() || gunEspNow.takeFire();`（本地微动 + ESP-NOW 双源） |
| loop 状态 | `gunPowerUpdate()`（断电/恢复）+ `gunEspNow.update()`（掉线监控） |
| config.h | `TRIGGER_BOARD_MAC`、`ESPNOW_CHANNEL` |

> 开火逻辑（consumeShot + IR 双帧 + 电机联动 + F 帧 + 音效）完全复用；
> 校准模式（菜单"校准激光"）仍走本地微动。

## 7. 待实测（下一步）

1. 多枪型电流波形采集 → 阈值/去抖/堵转参数校准（`TRIG_CURRENT_A` 等）
2. 正式板打样/焊接 → 验证电源链（XL1509 5V + AMS1117 3.3V）、USB-C 原生 USB 烧录、
   INA226（IO4/IO5，2mΩ）、UCC27517→20N03 断电回路
3. ESP-NOW 端到端延时统计（P95）、同场多把枪互扰、断连恢复
4. 断电响应时间（指令→断 <50ms）、断电后重上电副作用（枪自带电子件复位）
5. INA226 报警中断替代轮询（扳机零轮询延迟，优化项；Alert 接 IO1/IO2）
