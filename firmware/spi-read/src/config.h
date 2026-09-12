#pragma once

// ============================================================================
// spi-read —— 第二步：用 SPI 读 SX126x 的寄存器/参数，打印到串口
//
// 引脚与正式固件完全一致（gun 的 config.h / PCB 引脚映射表），只在这一处定义。
//
// 2026-09-12：网关板已改为 **与枪端主板完全相同的接线**（见 gateway/src/config.h），
// 因此不再区分两块板 —— 网关也烧 [env:gun-s3]（原 gw-esp32 env 已删除）。
// ============================================================================

#define BOARD_NAME "gun-board / gateway（ESP32-S3，接线相同）"
#define PIN_NSS   16
#define PIN_SCK   42
#define PIN_MOSI  15
#define PIN_MISO  41
#define PIN_RST   46
#define PIN_BUSY  45
#define PIN_LED   48
#define LED_ON_LEVEL LOW

// ---------------------------------------------------------------- 参数
#define SPI_HZ_DEFAULT 1000000UL   // 先用 1MHz（稳），按 s 逐档提到 16MHz
#define RST_LOW_MS     2           // 开机复位脉冲（SX126x 要求 >100µs）
#define BUSY_WAIT_US   3000        // 发命令前等 BUSY 变低的上限（超时只计数，不卡死）
#define REPORT_MS      2000        // 自动打印周期

// ---------------------------------------------------------------- SX126x 命令
// 出处：DS_SX1262.pdf 表 12-1 / §13（已与 RadioLib SX126x.h 对照一致）
#define CMD_SET_STANDBY        0x80
#define CMD_READ_REGISTER      0x1D
#define CMD_GET_STATUS         0xC0
#define CMD_GET_PACKET_TYPE    0x11
#define CMD_GET_RSSI_INST      0x15
#define CMD_GET_DEVICE_ERRORS  0x17

// ---------------------------------------------------------------- 寄存器
#define REG_VERSION_STRING     0x0320   // 16 字节 ASCII："SX1268"
#define REG_LORA_SYNC_MSB      0x0740   // 复位值 0x14（私有网 0x1424 / 公共网 0x3444）
#define REG_LORA_SYNC_LSB      0x0741   // 复位值 0x24
#define REG_RX_GAIN            0x08AC   // 复位值 0x94（boosted 0x96）
#define REG_OCP                0x08E7   // 与芯片相关：SX1262 常为 0x38 / SX1261 为 0x18
#define REG_XTA_TRIM           0x0911   // 复位值 0x05
#define REG_XTB_TRIM           0x0912   // 复位值 0x05
#define REG_RANDOM_GEN         0x0819   // 4 字节随机数（用于确认"读"是真的）
