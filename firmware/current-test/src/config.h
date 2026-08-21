#pragma once

// ============================================================
// current-test：电流测试专用配置
// 板卡：triger-sensor-current-test（实验板）
//   ESP32-S3 立创开发板 + INA226（I2C 地址 0x40）+ 2mΩ 采样电阻
//   原理图见 PCB/triger-sensor-current-test-新原理图分析.md
//
// 记录流程（自动循环，见 README.md）：
//   1) 开机测基础电流（空闲均值，电机断电时进行）
//   2) 电流 > max(2×基础电流, 1A) → 开始记录
//   3) 每 10ms 记录一次，连续 100 次（1s 窗口）
//   4) 记录满 100 次后一次性返回全部数据（#BLOCK begin ... #BLOCK end）
//   5) 电流回落到 1A 以下（持续 50ms）→ 重新武装，进入下一轮
// ============================================================

// ===== 串口 =====
#define SERIAL_BAUD 115200   // 数据块一次性返回需要高波特率；若只支持 115200
                             // 也可用（100 行 ≈ 1.4KB ≈ 120ms）

// ===== I2C / INA226 =====
#define PIN_SDA 17           // 原理图 G17（SDA，2.2kΩ 上拉 R2）
#define PIN_SCL 18           // 原理图 G18（SCL，2.2kΩ 上拉）
#define INA226_ADDR 0x40     // A0=A1=GND → 0x40
#define I2C_CLK_HZ 400000
#define SHUNT_MOHM 2         // 采样电阻 2mΩ（HoLLR2512-3W-2mR-1%，3W）

// INA226 转换时间码（VBUSCT/VSHCT 共用，见 datasheet）
//   0=140µs  1=204µs  2=332µs  3=588µs  4=1100µs
// 记录周期 10ms 远大于转换周期，此值只需保证触发轮询能读到新鲜数据
#define INA226_CONV_CODE 1

// ===== 记录参数 =====
#define RECORD_COUNT 100     // 连续记录次数（1s 窗口 = 100 × 10ms）
#define RECORD_DT_MS 10      // 记录间隔（ms）
#define TRIG_CURRENT_A 1.0f  // 触发/回落阈值：电流超过 1A（或 2×基础电流）开始记录；
                             // 发送完后电流回落到 1A 以下才进入下一轮
#define POLL_INTERVAL_US 2000// 等待/回落阶段的轮询间隔（触发检测时延 ≤2ms）
#define DROP_DEBOUNCE_MS 50  // 回落到阈值以下需持续 50ms 才确认（防抖）

// ===== 引脚 =====
#define PIN_LED 2            // 状态 LED（G02 → R3 220Ω → LED1；记录期间点亮）
