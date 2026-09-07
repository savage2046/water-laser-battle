#pragma once

// ============================================================
// trigger-board-selftest —— ESP32-C3 检测板（triger-sensor 正式板）硬件自检固件
//
// 目标：不加载正式 trigger-board 固件，单独用于出厂/维修时的硬件体检：
//   [1] 电流检测链路：INA226(0x40) + 2mΩ 分流 R1 + I2C(IO4/IO5) 是否工作
//   [2] ESP-NOW 无线连接：与对端（枪端主控 / espnow-verify / 另一块本固件板）
//       能否建立双向链路
// 附带：MOSFET 断电回路步进检查(T2)、扳机电流脉冲监控（命令 'c'）。
//
// 引脚与 triger-sensor 正式板原理图 / 正式固件 trigger-board/src/config.h
// 完全一致（IO4/IO5 = INA226，IO3 = UCC27517→Q1，IO0 = LED，IO18/19 = USB）。
// ============================================================

// ===== 引脚（triger-sensor 正式板，ESP32-C3-WROOM-02-N4）=====
#define PIN_SDA 4                 // INA226 I2C SDA
#define PIN_SCL 5                 // INA226 I2C SCL
#define INA226_ADDR 0x40          // A0/A1 接地
#define PIN_MOSFET 3              // IO3 → UCC27517.IN+ → OUT → Q1(20N03)；HIGH=枪回路导通
#define PIN_LED 0                 // IO0 → R6(220Ω) → LED1；高电平点亮

// ===== INA226 参数 =====
#define SHUNT_MOHM 2              // R1 = JER2512F3R002：2mΩ 3W 2512
#define INA_CFG 0x06C7            // 与正式固件一致：连续 shunt+bus、AVG=1、~1.2ms/周期
#define INA_CAL 0x0A00            // 校准寄存器 0x0A00 → 芯片 Current 寄存器 LSB = 1mA

// ===== T1 电流检测自检参数 =====
#define CS_SAMPLES 100            // 采样点数
#define CS_SAMPLE_GAP_MS 3        // 采样间隔
#define CS_CUR_STDEV_MAX_A 0.10f  // 空闲电流噪声上限（σ），超过判异常
#define CS_CUR_RANGE_MAX_A 60.0f  // 读数有效范围上限（2mΩ 理论最大 ±40.96A）
#define CS_CHIP_DIFF_MAX_A 0.05f  // 芯片 Current 与分流电压换算的最大允许偏差
#define CS_CAL_SAMPLES 64         // 空闲基线采样数

// ===== T2 MOSFET 回路步进自检参数 =====
#define T2_OFF_SAMPLE_MS 150      // 门极关断时采样时长
#define T2_ON_SETTLE_MS 300       // 门极导通后等待稳定
#define T2_ON_SAMPLE_MS 400       // 门极导通时采样时长
#define LOAD_STEP_MIN_A 0.2f      // ΔI ≥ 0.2A 判定导通/断电回路正常（需台架负载）

// ===== 扳机电流监控参数（命令 'c'；与正式固件 CurrentSense 一致）=====
#define TRIG_CURRENT_A 1.0f       // 高电流判定阈值
#define DEBOUNCE_MS 10            // 触发去抖
#define PULSE_MIN_GAP_MS 20       // 相邻脉冲最小间隔
#define STALL_MS 2000             // 持续高电流 = 堵转
#define END_MS 50                 // 低电流持续 = 一发射击结束
#define SENSE_INTERVAL_MS 1       // 采样周期（INA226 ~1.2ms 转换自然限速）

// ===== T3 ESP-NOW 连接自检参数（帧协议与正式固件一致）=====
#define ESPNOW_CHANNEL 1
#define FRAME_MAGIC 0xA5
#define FRAME_FIRE  0x01          // 开火脉冲（d0=序号）
#define FRAME_HB    0x02          // 心跳（d0=1s 计数）
#define FRAME_FAULT 0x03          // 异常
#define FRAME_POWER 0x10          // 断电/恢复指令（d0=0 断 / 1 通）
#define HB_INTERVAL_MS 1000       // 心跳间隔
#define LINK_STATUS_MS 2000       // 状态打印间隔
#define LINK_SETTLE_MS 10000      // 观察窗口：此后仍无确认 → 判无对端
#define MAX_PEERS 4               // 自动配对对端上限

// 对端 MAC（枪端主控 / 测试对端）：
//   FF:FF:FF:FF:FF:FF = 纯自动发现：先广播心跳；收到任何本协议合法帧后自动把
//                       发送方加为单播对端（可与另一块同固件板双向对测）。
//   填写真实 MAC      = 额外向该 MAC 单播心跳，靠 ESP-NOW 链路层 ACK 确认对端在场。
//                       例：espnow-verify 默认伪装成枪端 MAC
//                       { 0x24,0x0A,0xC4,0x00,0x00,0x01 }，双向对测可直接填它。
#define PEER_MAC { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

// ===== 串口 =====
#define SERIAL_BAUD 115200

// ===== 固件信息 =====
#define SELFTEST_VERSION "1.0.0"
