#pragma once

// ============================================================
// 无改装水弹枪联动——检测板配置（见 docs/trigger-sensor-study.md）
// 串联电池回路：电流检测扳机/连发 + MOSFET 断电 + ESP-NOW 连枪端主控
// ============================================================

// ===== 配对（与枪端主控互填对方 MAC）=====
#define BOARD_ID 1
#define GUN_MAC { 0x24, 0x0A, 0xC4, 0x00, 0x00, 0x01 }  // 枪端主控 MAC

// ===== 引脚 =====
#define PIN_SDA 21           // INA226 I2C SDA
#define PIN_SCL 22           // INA226 I2C SCL
#define INA226_ADDR 0x40
#define PIN_ACS712 34        // ACS712 模拟输出（纯输入引脚；INA226 方案下未用）
#define PIN_MOSFET 15        // 断电开关 MOSFET 栅极（HIGH=枪回路导通）
#define PIN_LED 2            // 状态 LED
// #define PIN_ALERT 4       // INA226 报警中断（预留：扳机零轮询触发）

// ===== 电流检测 =====
// 传感器二选一：INA226（推荐，I2C 数字）或 ACS712（模拟，快速原型）
#define CURRENT_SENSE_INA226 1
// #define CURRENT_SENSE_ACS712 1
#define SHUNT_MOHM 2         // INA226 分流电阻 2mΩ
#define TRIG_CURRENT_A 1.0f  // 扳机判定阈值（开机自校准后覆盖）
#define DEBOUNCE_MS 10       // 扳机去抖
#define PULSE_MIN_GAP_MS 20  // 相邻脉冲最小间隔（防抖）
#define STALL_MS 2000        // 持续高电流 = 堵转（过流保护断电）
#define END_MS 50            // 低电流持续 = 发射结束
#define SENSE_INTERVAL_MS 1  // 采样间隔（INA226 实际转换周期 ~1.2ms）

// ===== ESP-NOW（检测板 → 枪端）=====
#define ESPNOW_CHANNEL 1
#define FRAME_MAGIC 0xA5
#define FRAME_FIRE  0x01     // 开火脉冲（每发；d0=序号）
#define FRAME_HB    0x02     // 心跳（d0=1s 计数）
#define FRAME_FAULT 0x03     // 异常（d0=0x01 堵转保护断电）
#define FRAME_POWER 0x10     // 断电/恢复指令（枪端→检测板；d0=0 断 1 通）
#define HB_INTERVAL_MS 1000
