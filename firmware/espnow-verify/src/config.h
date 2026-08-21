#pragma once

// ============================================================
// espnow-verify：ESP-NOW 信号验证固件（接收电流检测板信号）
// 配套 firmware/trigger-board（检测板）使用，协议见 docs/trigger-board.md §4
// ============================================================

// ===== 串口 =====
#define SERIAL_BAUD 115200

// ===== ESP-NOW（与检测板 config.h 保持一致，勿单独改动）=====
#define ESPNOW_CHANNEL 1
#define FRAME_MAGIC 0xA5
#define FRAME_FIRE  0x01     // 开火脉冲（d0=序号）
#define FRAME_HB    0x02     // 心跳（d0=1s 计数）
#define FRAME_FAULT 0x03     // 异常（d0=0x01 堵转保护断电）
#define FRAME_POWER 0x10     // 断电/恢复指令（下行测试；d0=0 断 1 通）

// ===== 配对 =====
// 检测板把开火/心跳帧单播发往 GUN_MAC。默认开启 MAC 伪装（SPOOF_GUN_MAC 1）：
// 验证板开机时把自己 WiFi MAC 临时改成 GUN_MAC，无需改动检测板固件即可收到
// 其单播帧（同时也能收到广播帧）。重启即还原，不改动任何持久配置。
#define SPOOF_GUN_MAC 1
#define GUN_MAC { 0x24, 0x0A, 0xC4, 0x00, 0x00, 0x01 }  // 检测板 config.h 的 GUN_MAC

// 下行测试（发送 FRAME_POWER）的目标：
//   默认广播 FF:FF:FF:FF:FF:FF —— 检测板能收到广播，无需知道其 MAC；
//   也可填检测板真实 MAC（单播，需先加 peer；MAC 见检测板开机串口或 esptool）。
#define BOARD_MAC { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

// ===== 判活 / 统计 =====
#define HB_TIMEOUT_MS 3000        // 心跳超时判离线（与枪端一致）
#define STATUS_INTERVAL_MS 2000   // 周期状态打印间隔
#define FIRE_RATE_WINDOW_MS 1000  // 射速统计窗口（最近 1s 内发数）
#define FIRE_RING_SIZE 64         // 开火时刻环形缓冲（射速/间隔统计）
#define SENDER_MAX 4              // 发送端 MAC 记录上限（多板同测）
#define FIRE_LED_MS 40            // 开火 LED 点亮时长（ms）

// ===== 引脚 =====
#define PIN_LED 2                 // 状态 LED：开火闪 FIRE_LED_MS；FAULT 常亮（'r' 熄灭）
