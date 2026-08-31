#pragma once

// ============================================================
// current-test：电流测试专用配置
// 板卡：triger-sensor-current-test（实验板）
//   ESP32-S3 立创开发板 + INA226（I2C 地址 0x40）+ 2mΩ 采样电阻
//   原理图见 PCB/triger-sensor-current-test-新原理图分析.md
//
// 记录流程（自动循环，见 README.md）：
//   1) 电流首次 > 1A → 激发开始（射击点 #FIRE START），开始记录
//   2) 每 1ms 记录一次（1000Hz）；电流 < 1A 持续 50ms → 激发停止，提前结束
//   3) 结束（回落或满 2000 点长按保护）后一次性返回数据（#BLOCK begin ... end）
//   4) 回到等待，循环检测下一次激发。负值不校准、忽略。
// ============================================================

// ===== 串口 =====
#define SERIAL_BAUD 921600   // 数据块一次性返回需要高波特率
                             // （2000 行 ≈ 28KB ≈ 0.3s @921600；115200 下约 2.4s）

// ===== I2C / INA226 =====
#define PIN_SDA 17           // 原理图 G17（SDA，2.2kΩ 上拉 R2）
#define PIN_SCL 18           // 原理图 G18（SCL，2.2kΩ 上拉）
#define INA226_ADDR 0x40     // A0=A1=GND → 0x40
#define I2C_CLK_HZ 400000
#define SHUNT_MOHM 2         // 采样电阻 2mΩ（HoLLR2512-3W-2mR-1%，3W）

// INA226 转换时间码（VBUSCT/VSHCT 共用，见 datasheet）
//   0=140µs  1=204µs  2=332µs  3=588µs  4=1100µs
// 记录周期 1ms 大于转换周期（约 0.41ms），读数新鲜；若需再快可改 code 0
#define INA226_CONV_CODE 1

// ===== 记录参数 =====
#define RECORD_COUNT 2000    // 连续记录次数（2s 窗口 = 2000 × 1ms，1000Hz）
#define RECORD_DT_US 1000    // 记录间隔（µs）= 1ms
#define TRIG_CURRENT_A 1.0f  // 触发/回落阈值：固定 1A（不测基础电流）——电流超过 1A
                             // 开始记录；发送完后电流回落到 1A 以下才进入下一轮
#define POLL_INTERVAL_US 500 // 等待/回落阶段的轮询间隔（触发检测时延 ≤0.5ms）
#define DROP_DEBOUNCE_MS 50  // 回落到阈值以下需持续 50ms 才确认（防抖）

// ===== 电流平滑（滑动平均低通，见 main.cpp smoothA）=====
// 1kHz 采样下点对点抖动 ±0.5~1A（周期 2~3ms），叠加在 30ms 真实射速谷底上。
// 滑动平均 N 点：压掉 <N·2ms 的高频抖动，保留慢变化谷底。
#define SMOOTH_N 5           // 窗口点数（5ms）。N=5：抖动被压掉≥60%，延迟≈2ms

// ===== 连发单发谷底检测（见 main.cpp detectValleyShot）=====
// 谷底判定：平滑后峰谷差 ≥ VALLEY_DROP_A 才计一发；drop < 阈值视为抖动。
// 注意：平滑同时削峰填谷，会把真实谷底 drop 压缩（0.5~0.7A 常见），
//       阈值需现场实测权衡：过低误报抖动、过高漏掉浅谷。
#define VALLEY_DROP_A 1.0f
#define SHOT_MIN_GAP_MS 30    // 相邻两发最小间隔（防单峰被噪声拆成两发；
                              // 30ms ≈ 33Hz 射速上限）

// ===== 引脚 =====
#define PIN_LED 2            // 状态 LED（G02 → R3 220Ω → LED1；记录期间点亮）
#define PIN_MOSFET 10        // 电机回路 MOSFET 使能（G10 → R5 → UCC27517 → Q1=20N03；
                             // HIGH=导通）。上电自动拉高，保证单独测试时电流流过采样电阻
