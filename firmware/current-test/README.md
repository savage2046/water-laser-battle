# current-test：电流测试专用固件（ESP32-S3 + INA226）

独立于 trigger-board 的**电流测试专用程序**：只做一件事——读取电流，并通过串口把
数据返回。采用**自动记录循环**：先测基础电流，电流超过阈值后按 10ms 间隔记录
100 次，一次性返回，然后等待电流回落再进入下一轮（避免持续全速上报导致的数据
超载）。

板卡：`triger-sensor-current-test` 实验板（原理图见 `PCB/triger-sensor-current-test-新原理图分析.md`）。

## 记录流程（自动循环）

```
开机 → 测基础电流(0.5s均值)
     → #ARMED（等待）
     → 电流 > max(2×基础电流, 1A) → #REC start，开始记录
     → 每 10ms 记录一次，连续 100 次（1s 窗口，LED 点亮）
     → #BLOCK begin ... 100 行数据 ... #BLOCK end（一次性返回）
     → #WAIT_DROP：电流回落到 1A 以下（持续 50ms）
     → #ARMED（重新武装）→ 下一轮
```

- **触发阈值** = `max(2×基础电流, 1A)`：例如基础电流 10mA → 阈值 1A；
  若基础电流 800mA → 阈值 1.6A。
- **发送阶段不做电流采样/记录**（`#BLOCK` 发送期间暂停采集，避免数据超载）；
  100 行 ≈ 1.4KB，921600 波特下约 15ms 发完。
- 记录不因电流回落而中断：一旦触发就连续采满 100 个点（含脉冲尾部），保证波形完整。

## 硬件接线（与原理图一致）

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| INA226 SDA | GPIO17 | I2C，2.2kΩ 上拉（R2） |
| INA226 SCL | GPIO18 | I2C，2.2kΩ 上拉 |
| INA226 地址 | 0x40 | A0=A1=GND |
| 采样电阻 | 2mΩ | HoLLR2512-3W-2mR-1%（`SHUNT_MOHM` 可改） |
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
#CAL measuring baseline (0.5s, 保持电机断电)...
#CAL done base=12.0 mA trig=1000 mA (max(2x base, 1000 mA))
#ARMED record=100 x 10ms  trig=1000 mA
#REC start
```

每轮记录返回（一次性、100 行）：

```
#BLOCK begin cycle=1 n=100 dt=10ms peak=1540.0 mA avg=310.0 mA
0,12
10,15
20,1240
...
990,18
#BLOCK end
#WAIT_DROP current < 1A ...
#ARMED cycle=1 base=12.0 mA trig=1000 mA
```

- 每行 `t_ms,I_mA`：`t_ms` 为相对触发时刻的毫秒数（0..990），`I_mA` 为电流毫安。
- 可直接用 Arduino 串口绘图器 / Python / Excel 解析 `#BLOCK begin` 与 `#BLOCK end`
  之间的数据行。Python 示例：

```python
import serial
ser = serial.Serial('COM3', 921600, timeout=1)
while True:
    line = ser.readline().decode(errors='ignore').strip()
    if line.startswith('#BLOCK begin'):
        data = []
        while True:
            l = ser.readline().decode(errors='ignore').strip()
            if l.startswith('#BLOCK end'):
                break
            if ',' in l:
                t, i = l.split(',')
                data.append((int(t), int(i)))
        print(f"cycle: {len(data)} samples, peak: {max(v for _, v in data)} mA")
```

## 串口命令

| 命令 | 说明 |
| --- | --- |
| `c` | 重测基础电流（**电机断电时执行**；记录/发送中会拒绝） |
| `q` / `v` | 查询状态：当前相位、基础电流、触发阈值、已发送轮次、Vbus/电流 |
| `d` | 读取 INA226 全部寄存器（诊断） |
| `?` / `h` | 帮助 |

## 测试流程建议

1. 烧录固件 → 打开串口监视器，确认 `vbus≈11000 mV`（3S 电池）与
   `#ARMED record=100 x 10ms`。
2. **电机断电**状态下发 `c` 重测基础电流（正常情况下应接近 0——检测板自身耗电
   不经过采样电阻，只有电机电流流经 R1）。
3. 扣动扳机（可连发）：自动开始记录并返回 `#BLOCK` 数据；每轮结束后等电流回落
   到 1A 以下自动武装，可连续扣扳机测多轮。
4. 如需调整：记录次数/间隔改 `RECORD_COUNT`、`RECORD_DT_MS`；触发/回落阈值改
   `TRIG_CURRENT_A`（config.h）。

## 常见问题

- **`#ERROR INA226 not detected`**：检查 SDA/SCL 是否接反、上拉电阻、I2C 地址
  （`d` 命令可看寄存器）；确认 3.3V/5V 供电正常。
- **一直不触发**：确认电机回路经 XT30（CN1 电机 / CN2 电池）闭合，且 MOSFET 导通
  （本测试板 G10 默认由 trigger-board 控制；单独测试时需手动拉高 G10 或短接 Q1，
  电流才能流过采样电阻）。
- **一轮结束后不进入下一轮**：`#WAIT_DROP` 等待电流回落到 1A 以下，若电机仍在
  运转（或堵转）会一直等待，属正常保护。
