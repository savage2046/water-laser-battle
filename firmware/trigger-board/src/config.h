#pragma once

// ============================================================
// 无改装水弹枪联动——检测板配置（见 docs/trigger-sensor-study.md）
// 串联电池回路：电流检测扳机/连发 + MOSFET 断电 + ESP-NOW 连枪端主控
// 正式板：PCB/triger-sensor.eprj2（ESP32-C3-WROOM-02-N4 + USB-C）
//   IO4/IO5 = INA226 I2C；IO3 = UCC27517 栅极驱动 → Q1(20N03)；
//   IO0 = LED；IO18/IO19 = 原生 USB（Type-C 烧录，免 CH340）
// ============================================================

// ===== 配对（与枪端主控互填对方 MAC）=====
#define BOARD_ID 1
#define GUN_MAC { 0x24, 0x0A, 0xC4, 0x00, 0x00, 0x01 }  // 枪端主控 MAC

// ===== 引脚（正式板 triger-sensor.eprj2，ESP32-C3）=====
#define PIN_SDA 4            // INA226 I2C SDA（原理图接 ESP32-C3 IO4）
#define PIN_SCL 5            // INA226 I2C SCL（原理图接 ESP32-C3 IO5）
#define INA226_ADDR 0x40
#define PIN_ACS712 6         // ACS712 模拟输出（备用方案；正式板 INA226 方案下未用）
#define PIN_MOSFET 3         // 断电开关控制（IO3 → U4.IN+ UCC27517 → OUT → Q1=20N03 栅极；HIGH=枪回路导通）
#define PIN_LED 0            // 状态 LED（IO0 → R6(220Ω) → LED1；高电平点亮）
// #define PIN_ALERT 1       // INA226 报警中断（预留：扳机零轮询触发；接 IO1/IO2）

// ===== 电流检测 =====
// 正式板固定 INA226 方案（R1 = JER2512F3R002，2mΩ 3W 2512）
#define CURRENT_SENSE_INA226 1
// #define CURRENT_SENSE_ACS712 1
#define SHUNT_MOHM 2         // INA226 分流电阻 2mΩ（原理图 R1 = JER2512F3R002，C46634447）
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
