# triger-sensor 检测板固件（ESP32-C3 独立工程）

> 对应正式板工程 `PCB/triger-sensor.eprj2`（嘉立创EDA专业版，原理图 INA226/P1）。
> 本工程为独立新建目录 `firmware/triger-sensor-esp32c3/`；如需维护既有工程请见
> `firmware/trigger-board/`（同为 ESP32-C3 方案，可二选一或对比使用）。

## 1. 硬件（triger-sensor.eprj2）

| 位号 | 器件 | 说明 |
| --- | --- | --- |
| U1 | ESP32-C3-WROOM-02-N4 | 主控模组（4MB flash，原生 USB-C = IO18/IO19） |
| U3 | INA226AIDGSR | 电流监视器，I2C 地址 0x40，A0/A1 接地 |
| R1 | JER2512F3R002（2mΩ 3W 2512） | 分流电阻，串联电池回路 |
| U4 + Q1 | UCC27517 + 20N03 | 栅极驱动 → N-MOS 低端断电开关 |
| U2 / U7 | XL1509-5.0 / AMS1117-3.3 | 电源链（11.1V→5V→3.3V） |
| USB1 | TYPE-C 16PIN | 原生 USB 烧录/供电（免 CH340） |

## 2. 引脚接线（与 2026-09-05 原理图网表一致）

| 信号 | 引脚 | 网络/路径 |
| --- | --- | --- |
| INA226 SDA | **IO4** | U1.IO4 ↔ R2(2.2kΩ 上拉) ↔ U3.SDA |
| INA226 SCL | **IO5** | U1.IO5 ↔ R3(2.2kΩ 上拉) ↔ U3.SCL |
| 断电开关 | **IO3** | IO3 → UCC27517.IN+ → OUT → Q1.G(20N03)；HIGH=枪回路导通 |
| 状态 LED | **IO0** | IO0 → R6(220Ω) → LED1 阳极；阴极接 GND；高电平点亮 |
| USB D- / D+ | **IO18 / IO19** | 原生 USB（Type-C 烧录，免 CH340） |
| INA226 分流 | 2mΩ | R1(JER2512F3R002)；VIN+=+11.1V、VIN-=NET2、VBUS=+11.1V |

> 电流计算：INA226 分流电压 LSB = 2.5µV；`I = V_shunt / 2mΩ`。
> INA226 Config=0x06C7（连续转换，周期 ≈1.18ms）。

## 3. 功能

- INA226 电流采样 + 扳机/连发识别（开机 500ms 空闲基线自校准）
- 每发开火 → `FRAME_FIRE` ESP-NOW 上报枪端（逐发同步音效/状态）
- 开火（发射状态）期间串口每 `CUR_LOG_INTERVAL_MS`(0.2s) 输出一次净电流：
  `[cur] I=xx.xxA`（已扣空闲基线；开火起始立即输出第一点，之后按周期）
- **三次触发自动断电**：每发计 1 次，累计满 `SHOT_LIMIT`(3) 发 → MOSFET 断电
  （模拟空弹/回合结束），延时 `AUTO_RECOVER_MS`(3s) 后自动恢复导通并清零计数；
  收到枪端 `FRAME_POWER` 指令时以指令为准（手动断电不自动恢复，恢复指令清零计数）
- 堵转（持续高电流 ≥2s）→ 自动断电 + `FRAME_FAULT` 上报
- 心跳 `FRAME_HB`（1Hz）；枪端 3s 无心跳判掉线
- 接收枪端 `FRAME_POWER`：击杀/空弹断电（防作弊硬保证），重生/装弹恢复
- LED 开火闪灯；默认枪回路导通

## 4. ESP-NOW 帧协议（与 firmware/gun/src/GunEspNow.* 一致）

帧：`{ 0xA5, type, d0, d1 }`（4 字节），白名单点对点，信道 1。

| 方向 | type | d0 | 说明 |
| --- | --- | --- | --- |
| 板→枪 | 0x01 FIRE | 序号 | 每发开火脉冲 |
| 板→枪 | 0x02 HB | 计数 | 1Hz 心跳 |
| 板→枪 | 0x03 FAULT | 0x01=堵转 / 0x02=N发打完自动断电 | 已自动断电；0x02 会延时自动恢复 |
| 枪→板 | 0x10 POWER | 0=断 1=通 | 断电/恢复指令 |

## 5. 编译 / 烧录

```bash
pio run -e esp32c3                    # 编译
pio run -e esp32c3 -t upload          # USB-C 烧录
pio device monitor -e esp32c3 -b 115200
```

配对：把枪端主控真实 MAC 填入 `src/config.h` 的 `GUN_MAC`；
把本板 MAC（烧录后串口 `esp_read_mac` 打印）填入枪端 `TRIGGER_BOARD_MAC`。
