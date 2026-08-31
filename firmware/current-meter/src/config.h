#pragma once

// ============================================================
// current-meter：电流 1Hz 简易测试固件配置
// 板卡：ESP32-S3 立创开发板 + INA226（I2C 地址 0x40）+ 2mΩ 采样电阻
//   原理图见 PCB/triger-sensor-current-test-新原理图分析.md
//
// 用途：单独测试程序——开机后每隔 1 秒从串口输出一个电流值，
//       用于电流检测链路（INA226 → I2C → 串口）的初步验证。
// ============================================================

// ===== 串口 =====
#define SERIAL_BAUD 115200

// ===== I2C / INA226 =====
#define PIN_SDA 17           // 原理图 G17（SDA，2.2kΩ 上拉 R2）
#define PIN_SCL 18           // 原理图 G18（SCL，2.2kΩ 上拉）
#define INA226_ADDR 0x40     // A0=A1=GND → 0x40
#define I2C_CLK_HZ 400000
#define SHUNT_MOHM 2         // 采样电阻 2mΩ（HoLLR2512-3W-2mR-1%，3W）

// INA226 转换时间码（VBUSCT/VSHCT 共用，见 datasheet）
//   0=140µs  1=204µs  2=332µs  3=588µs  4=1100µs
#define INA226_CONV_CODE 1   // 204µs → 转换周期 ≈0.41ms，远小于 1s 输出周期

// ===== 输出 =====
#define OUTPUT_INTERVAL_MS 1000  // 每 1 秒输出一个电流值
#define AVG_SAMPLES 8            // 每次输出前对 8 个采样取均值（降噪）

// ===== 引脚 =====
#define PIN_LED 2            // 状态 LED（G02 → R3 220Ω → LED1；每秒翻转）
#define PIN_MOSFET 10        // 电机回路 MOSFET 使能（G10 → R5 → UCC27517 → Q1=20N03；
                             // HIGH=导通）。上电自动拉高，保证单独测试时电流流过采样电阻
