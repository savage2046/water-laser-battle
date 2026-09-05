#pragma once

// ============================================================
// triger-sensor.eprj2 正式板（ESP32-C3-WROOM-02-N4）检测板配置
// 无改装水弹枪联动：电流检测扳机/连发 + MOSFET 物理断电 + ESP-NOW 上报
// 原理图 P1（INA226 sheet）见 PCB/triger-sensor.eprj2；
// 引脚依据 2026-09-05 版原理图网表（IO4/IO5/IO3/IO0，见 PCB/triger-sensor-解析报告.md）
// ============================================================

// ===== 配对（与枪端主控互填对方 MAC）=====
// 检测板作为 ESP-NOW 从机；把枪端主控（firmware/gun）的真实 MAC 填到这里。
// 枪端 config.h 的 TRIGGER_BOARD_MAC 需填本板 MAC（烧录后串口打印 esp_read_mac）。
#define BOARD_ID 1
#define GUN_MAC { 0x24, 0x0A, 0xC4, 0x00, 0x00, 0x01 }  // 枪端主控 MAC（占位，实测后替换）

// ===== 引脚（triger-sensor.eprj2，ESP32-C3 模组）=====
#define PIN_SDA 4            // INA226 I2C SDA（原理图 U1.IO4 → R2 上拉 → U3.SDA）
#define PIN_SCL 5            // INA226 I2C SCL（原理图 U1.IO5 → R3 上拉 → U3.SCL）
#define INA226_ADDR 0x40     // A0/A1 接地 → 0x40
#define PIN_MOSFET 3         // 断电开关（IO3 → UCC27517.IN+ → OUT → Q1=20N03 栅极；HIGH=枪回路导通）
#define PIN_LED 0            // 状态 LED（IO0 → R6(220Ω) → LED1 阳极；高电平点亮，阴极接 GND）
// #define PIN_ALERT 1       // INA226 报警中断（预留：扳机零轮询触发；接 IO1/IO2）
#define PIN_ACS712 2         // 备用模拟电流方案（ACS712 输出；正式板未用，仅保留编译开关）

// ===== 电流检测 =====
// 正式板固定 INA226 方案：R1 = JER2512F3R002（2mΩ 3W 2512，C46634447）
#define CURRENT_SENSE_INA226 1
// #define CURRENT_SENSE_ACS712 1
#define SHUNT_MOHM 2         // 分流电阻 2mΩ（= 0.002Ω）
#define TRIG_CURRENT_A 1.0f  // 扳机判定阈值（开机自校准扣除空闲基线后比较）
#define DEBOUNCE_MS 10       // 扳机上升去抖
#define PULSE_MIN_GAP_MS 20  // 相邻脉冲最小间隔（防抖/滤噪）
#define STALL_MS 2000        // 持续高电流 = 堵转（过流保护自动断电）
#define END_MS 50            // 低电流持续 = 发射结束
#define SENSE_INTERVAL_MS 1  // 采样间隔（INA226 实际转换周期 ~1.18ms 自然限速）

// ===== ESP-NOW（检测板 → 枪端主控）=====
#define ESPNOW_CHANNEL 1
#define FRAME_MAGIC 0xA5
#define FRAME_FIRE  0x01     // 开火脉冲（每发；d0=序号）
#define FRAME_HB    0x02     // 心跳（d0=1s 计数）
#define FRAME_FAULT 0x03     // 异常（d0=0x01 堵转保护断电）
#define FRAME_POWER 0x10     // 断电/恢复指令（枪端→检测板；d0=0 断 1 通）
#define HB_INTERVAL_MS 1000
