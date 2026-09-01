# current-test：激发事件检测固件（ESP32-S3 + INA226）

独立于 trigger-board 的**电流测试专用程序**，用于验证"电流 → 射击点"判定：
**电流首次超过 1A 即判定为激发开始（射击点），电流低于 1A（持续 50ms）即判定为
激发停止**，循环检测。每次激发期间以 1000Hz（1ms）记录电流波形并一次性返回，
供核对。**不测基础电流、不做 pre-trigger、负值不校准不参与判定**。

**电流先经滑动平均平滑**（`SMOOTH_N=5`，5ms 窗口低通）：1kHz 采样下的点对点抖动
（±0.5~1A、周期 2~3ms）被压掉，30ms 射速谷底保留；谷底检测与波形存储都用平滑值。

连发模式下扳机持续按住、电流始终 > 1A，**每一发通过电流波动的"谷底 → 上升沿"
检测**（`detectValleyShot`）：平滑后峰谷差 ≥ 0.8A 且距上一发 ≥ 30ms 即计一发（#SHOT）。

板卡：`triger-sensor-current-test` 实验板（旧实验板；已被正式板 `PCB/triger-sensor.eprj2` 取代，
解析报告见 `PCB/triger-sensor-解析报告.md`；本实验板旧原理图分析见 git 历史 `PCB/triger-sensor-current-test-新原理图分析.md`）。

## 激发判定规则（射击点）

```
电流首次 > 1A  ──► 激发开始（#FIRE START，射击点）
电流 < 1A 持续 50ms ──► 激发停止（#FIRE STOP），回到等待，循环
```

- **激发开始 = 射击点**：WAIT 阶段轮询（0.5ms 节拍）检测到第一次 `电流 > 1A`
  即输出 `#FIRE START`，同时开始记录波形。不需要 pre-trigger（不抓上升沿前的数据）。
- **激发停止**：记录期间电流回落到 1A 以下并持续 50ms（`DROP_DEBOUNCE_MS` 防抖），
  提前结束本轮并输出 `#FIRE STOP`（含时长/峰值/均值/发数）。
- **负值不处理**：INA226 零点噪声（约 -10~-20mA）不校准、不滤除、不参与任何判定
  （负值 < 1A，天然属于"停止"一侧）。
- **长按保护**：电流持续 > 1A 满 2s（2000 点）仍不回落，强制结束本轮，等待回落
  后再武装，避免堵转/长按卡死。

## 连发单发检测（detectValleyShot）

连发时扳机不松开，电流不回落，此时"每一发的开始"表现为电流波动的谷底后上升沿：

```
下降沿（iA < prev）→ 记录谷底最低点 g_vValleyA 与下降起点 g_vDropFromA
开始上升（iA > prev）→ 确认谷底：峰谷差 = g_vDropFromA − g_vValleyA
   峰谷差 ≥ VALLEY_DROP_A 且距上一发 ≥ SHOT_MIN_GAP_MS → #SHOT（一发）
```

- 在 RECORD 阶段每次采样后调用；仅在激发区间（电流 > 1A）内检测，
  跌到 1A 以下（停止段/负值）自动重置，不会把"松扳机停机"误判成一发。
- **启动段抑制（方案A）**：激发开始后电流冲上浪涌峰值（可达 20A+），此段回弹会
  形成假谷底（误判成一发）。固件跟踪本轮峰值 `g_vSurgePeakA`，**电流从峰值回落
  到 ≤ 峰值×50% 之前不做谷底检测**——启动段假谷底从源头被消灭（实测：5burst
  数据 t=58ms 的假谷底被剔除，4 发干净保留）。
- **每一发均由谷底确认（无预置 n1）**：#FIRE START 仅标记激发开始、不计数；
  发数完全由谷底检测确认，n=1 即第一个谷底（启动段抑制后）——`#FIRE STOP`
  的 `shots` 就是本激发谷底确认的**总发数**。
- 调参（config.h）：`VALLEY_DROP_A=0.8f`（**平滑后**峰谷差阈值：drop < 0.8A 视为
  电流抖动，不算一发；平滑会削峰填谷，阈值需现场实测权衡——过低误报抖动、
  过高漏掉浅谷）、`SHOT_MIN_GAP_MS=30`（最小间隔，≈33Hz 射速上限，防单峰拆两发）、
  `SMOOTH_N=5`（滑动平均窗口，越大越平滑但谷底也削得越多）。

## 记录流程（自动循环）

```
开机 → #ARMED（等待）
     → 电流首次 > 1A → #FIRE START（射击点），开始记录（LED 点亮）
     → 每 1ms 采样一次（1000Hz），同时做谷底检测
     → 每确认一个谷底上升沿 → #SHOT n=... t=... ms（连发每一发）
     → 电流 < 1A 持续 50ms → #FIRE STOP → 发送数据
     → #BLOCK begin ... n 行数据 ... #BLOCK end（一次性返回，期间不采样）
     → #ARMED（重新武装）→ 下一轮
```

- **触发阈值固定 1A**（`TRIG_CURRENT_A`，config.h）。
- **采样率 1000Hz**（`RECORD_DT_US=1000`，config.h）：INA226 转换周期约 0.41ms，
  1ms 间隔下读数新鲜；如需更高需改 I2C 时钟与转换码（上限约 3~5kHz）。
- **发送阶段不做电流采样/记录**（`#BLOCK` 发送期间暂停采集，避免数据超载）；
  最多 2000 行 ≈ 28KB，921600 波特下约 0.3s 发完。
- 波形记录不因电流波动中断：只有回落 1A 以下持续 50ms 才结束本轮。

## 硬件接线（与原理图一致）

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| INA226 SDA | GPIO17 | I2C，2.2kΩ 上拉（R2） |
| INA226 SCL | GPIO18 | I2C，2.2kΩ 上拉 |
| INA226 地址 | 0x40 | A0=A1=GND |
| 采样电阻 | 2mΩ | HoLLR2512-3W-2mR-1%（`SHUNT_MOHM` 可改） |
| 电机回路使能 | GPIO10 | MOSFET 栅极驱动（HIGH=导通）；**上电自动拉高** |
| 状态 LED | GPIO2 | 记录期间点亮 |
| 串口 | USB 桥（U0TXD/U0RXD） | 波特率 921600 |

## 构建与烧录

```bash
cd firmware/current-test
pio run -t upload
pio device monitor          # 波特率 921600（platformio.ini 已配好）
```

> 若板子**没有 USB-UART 桥、只引出原生 USB**（GPIO19/20）：`platformio.ini` 加
> `upload_protocol = esp-builtin`，并把代码里的 `Serial` 换成 `USBSerial`
> （或 `Serial0`→`Serial` 的映射，取决于 Arduino 版本）。

## 串口输出格式

启动与事件（`#` 开头，可过滤）：

```
===== current-test : INA226 current recorder =====
#shunt=2 mOhm  vbus=11000 mV
#ARMED record=2000 x 1000us  trig=1.000 A
#FIRE START t=1234 ms I=3.342 A      ← 射击点（第一次电流 > 1A，仅标记，不计数）
#REC start
#SHOT n=1 t=1270 ms I=6.514 A (drop=2.03 A)   ← 每一发均由谷底确认（n 从 1 起）
#SHOT n=2 t=1302 ms I=5.760 A (drop=1.59 A)
...
```

每次激发结束返回（一次性、n 行，n = 实际记录点数）：

```
#FIRE STOP t=3456 ms dur=2222 ms peak=18.421 A avg=7.850 A shots=63 freq=32.4 Hz n=889
#BLOCK begin cycle=1 n=889 dt=1000us peak=18.421 A avg=7.850 A
>A:3.342
>A:18.421
...
>A:-0.011
#BLOCK end
#ARMED cycle=1 trig=1.000 A
```

- `#FIRE START t=... I=...`：**射击点**——首次电流 > 1A 的时刻（相对开机 ms）与
  电流值；**仅标记激发开始，不计数**。
- `#SHOT n=... t=... I=... (drop=...)`：**每一发（n 从 1 起）**——谷底确认并开始
  上升时输出，n 为本激发内发序号（n=1 即第一个谷底），t 为时刻，I 为上升点电流，
  drop 为峰谷差（A）。
- `#FIRE STOP t=... dur=... peak=... avg=... shots=... freq=... n=...`：激发结束——
  结束时刻、激发持续时长（ms）、峰值电流、平均电流、**本激发总发数 shots**、
  **稳定射速 freq**、记录点数。
- **稳定射速 freq 的计算**：扣除前两发（n1 启动第一发、n2 启动后不稳定发），用
  n3 至最后一发的间隔计算：`freq = (shots−3) × 1000 / (tLast − t3)` Hz。
  例：shots=63 → 稳定段 n3..n63 = 61 发、60 个间隔，除以其总时长即每秒发数。
  若 shots < 3（无稳定段），freq = 0 Hz。
- 数据行 `>A:I_A`：电流（安培，保留 3 位小数），1ms 间隔；n 随激发时长变化
  （如 889 点 ≈ 2.22s，时间由行序推算）。
- 可直接用 Arduino 串口绘图器 / Python / Excel 解析 `#BLOCK begin` 与 `#BLOCK end`
  之间的数据行。Python 示例：

```python
import serial
ser = serial.Serial('COM3', 921600, timeout=1)
while True:
    line = ser.readline().decode(errors='ignore').strip()
    if line.startswith('#FIRE START'):
        t = line.split()[2]          # 形如 "t=1234" → 射击点时刻
        print("fire at", t)
    if line.startswith('#SHOT'):
        print("shot:", line)         # 连发每一发
    if line.startswith('#BLOCK begin'):
        data = []
        while True:
            l = ser.readline().decode(errors='ignore').strip()
            if l.startswith('#BLOCK end'):
                break
            if l.startswith('>A:'):
                data.append(float(l[3:]))       # 形如 ">A:1.240"
        print(f"cycle: {len(data)} samples, peak: {max(data):.3f} A")
```

## 串口命令

| 命令 | 说明 |
| --- | --- |
| `q` / `v` | 查询状态：当前相位、触发阈值、已发送轮次、Vbus/电流 |
| `d` | 读取 INA226 全部寄存器（诊断） |
| `?` / `h` | 帮助 |

## 测试流程建议

1. 烧录固件 → 打开串口监视器，确认 `vbus≈11000 mV`（3S 电池）与
   `#ARMED record=2000 x 1000us`。
2. 扣动扳机（可连发）：电流首次 > 1A 立即输出 `#FIRE START`（射击点）；松扳机后
   电流回落 < 1A 持续 50ms 输出 `#FIRE STOP`，随后返回 `#BLOCK` 波形。连续扣扳机
   可测多轮，每轮对应一次激发。
3. 核对：`#FIRE START` 时刻应与你扣扳机的瞬间对应；`dur` 应约等于你按住扳机的
   时长。若频繁出现"刚触发就结束"或"触发后立即回落"，检查 `DROP_DEBOUNCE_MS`
   与触点/电机负载。
4. 如需调整：采样间隔改 `RECORD_DT_US`（上限保护次数 `RECORD_COUNT`）；阈值改
   `TRIG_CURRENT_A`；回落防抖改 `DROP_DEBOUNCE_MS`（config.h）。

## 常见问题

- **`#ERROR INA226 not detected`**：检查 SDA/SCL 是否接反、上拉电阻、I2C 地址
  （`d` 命令可看寄存器）；确认 3.3V/5V 供电正常。
- **一直不触发（无 #FIRE START）**：确认电机回路经 XT30（CN1 电机 / CN2 电池）
  闭合。本固件**上电时已自动拉高 G10**（`PIN_MOSFET=10`，HIGH=导通）使 MOSFET
  导通，电流即可流过采样电阻；若 G10 需要由 trigger-board 控制，可注释掉
  `main.cpp` 里的 MOSFET 初始化两行。
- **#FIRE START 后很久才 #FIRE STOP / 波形超长**：电流持续 > 1A（长按或堵转），
  达到 2000 点（2s）上限强制结束，属保护行为；堵转请断电检查电机。
- **一轮结束后不进入下一轮**：`#WAIT_DROP` 等待电流回落到 1A 以下，若电机仍在
  运转（或堵转）会一直等待，属正常保护。
