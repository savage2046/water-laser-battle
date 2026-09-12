#pragma once

// ============================================================================
// spi-probe v2 —— 极简 SPI 接口测试：只有引脚和默认参数
//
// 引脚与正式固件完全一致（不许改）：
//   枪端主板 firmware/gun/src/config.h + PCB/gun-board/main-board-S3引脚映射.md
//   网关板   firmware/gateway/src/config.h（2026-09-12 起与枪端接线相同）
// ============================================================================

#define BOARD_NAME "gun-board / gateway（ESP32-S3，接线相同）"
#define PIN_NSS   16
#define PIN_SCK   42
#define PIN_MOSI  15
#define PIN_MISO  41
#define PIN_RST   46
#define PIN_BUSY  45
#define PIN_LED   48          // 板载 LED 灌电流：低电平点亮
#define LED_ON_LEVEL LOW

// ---------------------------------------------------------------- 默认参数
// 时钟默认 250kHz：1 个时钟 4µs，示波器（10µs/格）能一格一格看清
#define SPI_HZ_DEFAULT      250000UL
// 帧间隔默认 100ms：NSS 高电平空档够长，触发稳、波形不会连成一片
#define FRAME_PERIOD_MS_DEFAULT 100
// 发命令前等 BUSY 变低的上限（超时就照发，不卡死）
#define BUSY_WAIT_US        3000

// ---------------------------------------------------------------- SX126x 命令
#define CMD_GET_STATUS      0xC0
#define CMD_READ_REGISTER   0x1D
#define REG_VERSION_STRING  0x0320
