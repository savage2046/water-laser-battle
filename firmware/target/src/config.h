#pragma once

// ===== 设备身份 =====
#define DEVICE_ID "TARGET1"
#define FW_VERSION "1.0.0"

// ===== 阵列接收（9 颗 TSOP38238，940nm/38kHz，仅载波检测）=====
#define ARRAY_COUNT 9
// 阵列引脚：左→右
#define ARRAY_RX0 26
#define ARRAY_RX1 25
#define ARRAY_RX2 36
#define ARRAY_RX3 39
#define ARRAY_RX4 34   // 中心颗（兼做 940nm 帧解码）
#define ARRAY_RX5 35
#define ARRAY_RX6 14
#define ARRAY_RX7 13
#define ARRAY_RX8 12
// 中心颗引脚（帧解码，与 ARRAY_RX4 相同）
#define PIN_IR_RX_940 34

// ===== 850nm 接收（TSOP4856，56kHz，帧解码）=====
#define PIN_IR_RX_850 27

// ===== 显示屏 =====
#define PIN_OLED_SDA 21
#define PIN_OLED_SCL 22
#define OLED_ADDR 0x3C

// ===== 蜂鸣器 =====
#define PIN_BUZZER 33

// ===== 红外解码参数（与枪端一致，双波段并行 24bit 半帧）=====
#define IR_PREAMBLE_US 9000
#define IR_PREAMBLE_GAP_US 4500
#define IR_BIT_HIGH_US 560
#define IR_BIT_ONE_GAP_US 1690
#define IR_BIT_ZERO_GAP_US 560
#define IR_PULSE_TOL_US 300
#define IR_GAP_THRESH_US 1125

// ===== 显示与判定 =====
#define DISPLAY_REFRESH_MS 150   // OLED 刷新间隔
#define HIT_WINDOW_MS 600        // 一次命中后阵列标志保持窗口
#define STRONG_FRAMES 2          // 收到 2 帧（双帧）= 强
