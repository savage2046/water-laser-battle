#pragma once

// ============================================================================
// wire-probe —— 只有一个功能：开机后 4 个管脚持续输出固定频率的方波
//
//   NSS = 10Hz    SCK = 100Hz    MOSI = 500Hz    RST = 1000Hz
//
// 不用 SPI、不用任何库；引脚与正式固件一致（gun/gateway 的 config.h）。
//
// 2026-09-12：网关板已改为 **与枪端主板完全相同的接线**（见 gateway/src/config.h），
// 因此不再区分两块板 —— 网关也烧 [env:gun-s3]（原 gw-esp32 env 已删除）。
// ============================================================================

#define BOARD_NAME "gun-board / gateway（ESP32-S3，接线相同）"
#define PIN_NSS   16
#define PIN_SCK   42
#define PIN_MOSI  15
#define PIN_RST   46
#define PIN_LED   48          // 板载 LED 灌电流：低电平点亮
#define LED_ON_LEVEL LOW

// 定时器时基：250µs（4kHz）。10/100/500/1000Hz 都是它的整数分频，频率精确。
#define TICK_US 250
