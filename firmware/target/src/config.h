#pragma once

// ===== 设备身份 =====
#define DEVICE_ID "TARGET1"
#define FW_VERSION "2.0.0"

// ============================================================
// 校准靶 v3：5×3 二维阵列（光斑 2D 位置/形状检测）
//   - 15× 38kHz IRM：XL-IRM-V838M3/TR（成兴光 C51900936，940nm 峰值）
//     5 列 × 3 行，间距 20mm → 靶面 100×40mm
//   - 15× WS2812B(5V) 命中灯：每格一颗，GPIO→74HCT1G125→5V DIN
//   - 主控：ESP32-S3（LCKFB-ESP32S3R8N8 / 任意 S3 板，GPIO 号通用）
//   行主序索引：idx = row*ARRAY_COLS + col（row0..2, col0..4）
//   中心格 idx=7（row1,col2）兼做 38k 帧解码（FrameRx 通道 0）
//   GPIO 分配（避开 0/3/19/20/26-37/45/46）：
//     15 IRM 输入: GPIO 1,2,4,5,6,7,8,9,10,11,12,13,14,15,16（行主序）
//     中心解码复用 idx7 = GPIO9
//     56k 预留: GPIO40（暂不贴件）  OLED: 21/38  蜂鸣: 39  灯: 17
// ============================================================

// ===== 阵列（5 列 × 3 行）=====
#define ARRAY_COLS 5
#define ARRAY_ROWS 3
#define ARRAY_COUNT (ARRAY_COLS * ARRAY_ROWS)  // 15
#define ARR_CENTER_IDX ((ARRAY_ROWS / 2) * ARRAY_COLS + (ARRAY_COLS / 2))  // 7

// 行主序引脚：idx0..14 = (row0..2 × col0..4)
#define ARR_RX0 1
#define ARR_RX1 2
#define ARR_RX2 4
#define ARR_RX3 5
#define ARR_RX4 6
#define ARR_RX5 7
#define ARR_RX6 8
#define ARR_RX7 9    // 中心格（row1,col2），兼帧解码
#define ARR_RX8 10
#define ARR_RX9 11
#define ARR_RX10 12
#define ARR_RX11 13
#define ARR_RX12 14
#define ARR_RX13 15
#define ARR_RX14 16

// 中心格解码引脚（= ARR_RX7）
#define PIN_IR_RX_940 9

// ===== 56kHz 近距通道（暂缓：预留焊盘不贴件；历史命名 _850）=====
#define PIN_IR_RX_850 40

// ===== 显示屏（SSD1306 128x64，软件 I2C）=====
#define PIN_OLED_SDA 21
#define PIN_OLED_SCL 38
#define OLED_ADDR 0x3C

// ===== 蜂鸣器 =====
#define PIN_BUZZER 39

// ===== 阵列命中指示灯：15× WS2812B(5V)，与 15 格一一对应 =====
#define PIN_LED_DATA 17           // 3.3V 数据 → 74HCT1G125 → 5V DIN（勿用 AO3400A 转换）
#define LED_COUNT 15              // 行主序串接（row0..2 × col0..4）
#define LED_HIT_COLOR 0x00FF00    // 命中绿
#define LED_IDLE_DIM 4            // 空闲微亮（0~255；0=全灭）
#define LED_CENTER_FLASH_MS 300   // 中心格解码命中白闪时长

// ===== 红外解码参数（与枪端一致，NEC 风格）=====
#define IR_PREAMBLE_US 9000
#define IR_PREAMBLE_GAP_US 4500
#define IR_BIT_HIGH_US 560
#define IR_BIT_ONE_GAP_US 1690
#define IR_BIT_ZERO_GAP_US 560
#define IR_PULSE_TOL_US 300
#define IR_GAP_THRESH_US 1125

// ===== 显示与判定 =====
#define DISPLAY_REFRESH_MS 60     // OLED/LED 刷新间隔（更短便于旋转/扫掠捕捉）
#define HIT_WINDOW_MS 800         // 一次命中后格标志保持窗口
#define STRONG_FRAMES 2           // 收到 2 帧（双帧）= 强
