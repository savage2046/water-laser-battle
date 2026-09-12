#pragma once

// ============================================================================
// lora-tx —— 只发不收：把 SX1268 配成 LoRa 470MHz，按周期发测试包
//
// 引脚与正式固件完全一致（gun 的 config.h / PCB 引脚映射表）。
// 射频参数与 lora-rx（收端）、正式固件完全一致，便于逐行对照。
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

// ---------------------------------------------------------------- SPI / 时序
#define SPI_HZ_DEFAULT   1000000UL
#define RST_LOW_MS       2
#define BUSY_WAIT_US     3000
#define TX_PERIOD_MS     1000      // 默认每秒发一包（按 p 可切 200/500/1000/2000）
#define TX_SUM_MS        5000      // 每 5 秒打一行汇总

// ---------------------------------------------------------------- 射频参数
// 与 lora-rx 及正式固件（firmware/gun/src/config.h）完全一致
#define RADIO_FREQ_MHZ   470.0f
#define LORA_SF          0x07      // 表 13-47：0x07 = SF7
#define LORA_BW          0x06      // 表 13-48：0x06 = 500 kHz
#define LORA_CR          0x01      // 表 13-49：0x01 = 4/5
#define LORA_LDRO        0x00      // 表 13-50：0 = 低数据率优化关
#define LORA_PREAMBLE    4         // 表 13-66：前导长度（码元）
#define LORA_HEADER      0x00      // 表 13-67：0 = 显式包头
#define LORA_PAYLOAD_LEN 8         // 发送载荷长度（字节）
#define LORA_CRC         0x01      // 表 13-69：1 = CRC 打开
#define LORA_IQ          0x00      // 表 13-70：0 = 标准 IQ
#define TX_POWER_DBM     0x0F      // SetTxParams 的功率参数 = +15dBm（与正式固件 RADIO_TX_POWER_DBM 15 一致）
#define TX_RAMP          0x04      // 表 13-41：0x04 = 200µs 上升时间

// ---------------------------------------------------------------- SX126x 命令
// 出处：DS_SX1262.pdf §13（括号内为手册表号）
#define CMD_SET_STANDBY        0x80   // 表 13-5
#define CMD_CLEAR_IRQ          0x02   // 表 13-31
#define CMD_SET_DIO_IRQ_PARAMS 0x08   // 表 13-28：IrqMask ★必须发（默认全屏蔽）
#define CMD_WRITE_BUFFER       0x0E   // 表 13-26
#define CMD_READ_REGISTER      0x1D   // 表 13-25
#define CMD_GET_STATS          0x10   // 表 13-83
#define CMD_GET_PACKET_TYPE    0x11   // 表 13-39
#define CMD_GET_IRQ_STATUS     0x12   // 表 13-30
#define CMD_GET_DEVICE_ERRORS  0x17   // 表 13-84
#define CMD_SET_TX             0x83   // 表 13-6
#define CMD_SET_RF_FREQUENCY   0x86   // 表 13-36
#define CMD_SET_PACKET_TYPE    0x8A   // 表 13-37
#define CMD_SET_MODULATION     0x8B   // 表 13-42
#define CMD_SET_PACKET_PARAMS  0x8C   // 表 13-51
#define CMD_SET_TX_PARAMS      0x8E   // 表 13-40：功率 + 上升时间
#define CMD_SET_BUFFER_BASE    0x8F   // 表 13-75
#define CMD_SET_PA_CONFIG      0x95   // 表 13-20/13-21：PA 配置（不配 = 默认低功率 PA）
#define CMD_SET_DIO2_RF_SWITCH 0x9D   // 表 13-32/13-33：DIO2 作 RF 开关（RX=0/TX=1）★必须发
#define CMD_GET_STATUS         0xC0   // 表 13-78

#define REG_VERSION_STRING     0x0320
#define REG_LORA_SYNC_MSB      0x0740
#define REG_LORA_SYNC_LSB      0x0741
