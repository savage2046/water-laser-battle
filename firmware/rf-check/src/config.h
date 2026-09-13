#pragma once

// ===== 硬件（枪端板 / 网关板 2026-09-12 起接线相同）=====
#define BOARD_NAME "gun/gateway board (ESP32-S3)"
#define PIN_NSS   16
#define PIN_SCK   42
#define PIN_MOSI  15
#define PIN_MISO  41
#define PIN_RST   46          // 低有效复位
#define PIN_BUSY  45          // 模组输出
#define PIN_DIO1   4          // 中断输出（体检项 [7] 会看它有没有翻转）
#define PIN_LED   48          // 模组板载 LED（灌电流：低电平点亮）
#define LED_ON_LEVEL LOW

// ===== LoRa 参数（与 lora-tx / lora-rx / 正式固件一致）=====
// 码值全部来自 DS_SX1262.pdf，表号见 lora-tx/src/config.h
#define RF_FREQ_MHZ      470.0f
#define LORA_SF          0x07    // 表 13-47：SF7
#define LORA_BW          0x06    // 表 13-48：500 kHz
#define LORA_CR          0x01    // 表 13-49：4/5
#define LORA_LDRO        0x00    // 表 13-50：关
#define LORA_PREAMBLE    4       // 表 13-66：前导 4 码元
#define LORA_HEADER      0x00    // 表 13-67：显式包头
#define LORA_CRC         0x01    // 表 13-69：CRC 开
#define LORA_IQ          0x00    // 表 13-70：标准 IQ
#define TX_POWER_DBM     15      // 表 13-40：合规 17dBm e.r.p.
#define TX_RAMP          0x04    // 200us

// ===== 超时 =====
#define BUSY_TIMEOUT_MS  50      // 等 BUSY 释放的上限
#define SPI_HZ           2000000 // SX126x 上限 16MHz，2MHz 稳
