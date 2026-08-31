# current-meter：电流 1Hz 简易测试固件（ESP32-S3 + INA226）

独立于 trigger-board / current-test 的**单独测试程序**：只做一件事——开机后
**每隔 1 秒读取一次电流，并从串口输出一个电流值**。用于电流检测链路
（INA226 → I2C → 串口）的初步验证，不需要触发、连发识别等逻辑。

> 与 `current-test` 的区别：`current-test` 是触发式记录仪（电流超阈值后以 10ms
> 间隔记录 100 个点一次性返回）；本程序是 1Hz 慢速巡检，格式最简单，适合刚
> 上电先确认"能不能读到电流、数值是否合理"。

## 硬件接线（与原理图一致）

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| INA226 SDA | GPIO17 | I2C，2.2kΩ 上拉（R2） |
| INA226 SCL | GPIO18 | I2C，2.2kΩ 上拉 |
| INA226 地址 | 0x40 | A0=A1=GND |
| 采样电阻 | 2mΩ | HoLLR2512-3W-2mR-1%（`SHUNT_MOHM` 可改） |
| 电机回路使能 | GPIO10 | MOSFET 栅极驱动（HIGH=导通）；**上电自动拉高** |
| 状态 LED | GPIO2 | 每秒翻转一次（呼吸指示） |
| 串口 | USB 桥（U0TXD/U0RXD） | 波特率 115200 |

## 构建与烧录

```bash
cd firmware/current-meter
pio run -t upload
pio device monitor          # 波特率 115200（platformio.ini 已配好）
```

> 若板子**没有 USB-UART 桥、只引出原生 USB**（GPIO19/20）：`platformio.ini` 加
> `upload_protocol = esp-builtin`，并把代码里的 `Serial` 换成 `USBSerial`。

## 串口输出格式

开机：

```
===== current-meter : INA226 1Hz current readout =====
#shunt=2 mOhm  vbus=11000 mV
#output: one current value every 1000 ms
#format: t_ms,I_A
```

之后每 1 秒一行（示例，电机未转 / 转动时）：

```
1000,0.012 A
2000,1.540 A
3000,1.505 A
...
```

- `t_ms`：开机后累计毫秒数（每秒 +1000）。
- `I_A`：电流（安培，保留 3 位小数）。每次输出前对 8 个采样取均值降噪
  （`AVG_SAMPLES` 可调）。
- 状态 LED 每秒翻转一次，肉眼确认程序在跑。

## 常见问题

- **串口无输出**：检查波特率是否 115200、板子是否处于烧录后复位状态。
- **`#ERROR INA226 not detected`**：检查 SDA/SCL 是否接反、上拉电阻、I2C 地址
  （应为 0x40）、供电。
- **电流一直为 0**：检查电机回路经 XT30 是否闭合。本固件**上电时已自动拉高
  G10**（`PIN_MOSFET=10`，HIGH=导通）使 MOSFET 导通，电流即可流过采样电阻；
  若 G10 需要由 trigger-board 控制，可注释掉 `main.cpp` 里的 MOSFET 初始化两行。
