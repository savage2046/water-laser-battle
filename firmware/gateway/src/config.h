#pragma once

// ===== 网关身份 =====
#define GATEWAY_ID "GW1"         // 每台网关唯一
#define GATEWAY_NAME "基地台1"
#define FW_VERSION "1.0.0"

// ===== WiFi =====
#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-pass"

// ===== 服务器（WebSocket）=====
#define SERVER_HOST "192.168.1.100"
#define SERVER_PORT 3000
#define WS_PATH "/ws"

// ===== SX1262 470MHz LoRa（SPI 直驱）=====
#define PIN_SX_NSS 5             // SPI 片选
#define PIN_SX_SCLK 18           // SPI 时钟
#define PIN_SX_MOSI 23           // SPI 主机输出
#define PIN_SX_MISO 19           // SPI 主机输入
#define PIN_SX_RST 16            // 复位
#define PIN_SX_BUSY 17           // 忙指示
#define PIN_SX_DIO1 4            // 中断（RadioLib 轮询模式可不接）

#define RADIO_FREQ_MHZ 470.0f    // 全队一致：470.0MHz
#define RADIO_BW_KHZ 500.0f      // 目标：SF7/500k（见 docs/wireless-research.md）
#define RADIO_SF 7
#define RADIO_CR 5
#define RADIO_SYNC_WORD 0x12     // 私有网络同步字（0x12=私有，0x34=公共LoRaWAN）
#define RADIO_TX_POWER_DBM 15    // 合规 17dBm e.r.p.（15dBm 传导 + 2dBi 天线）
#define RADIO_PREAMBLE_LEN 4     // 短前导：腾出时隙预算

// ===== TDMA MAC（自适应时隙 + 自动分配信道，见 docs/tdma-mac.md）=====
#define TDMA_ENABLE 1
#define GW_RF_SLOTS 12           // 射频槽位数（开机自检：实际装几个用几个，没装满自动兼容）
#define TDMA_MAX_SLOTS 17        // 每信道最大设备时隙（+1 注册时隙 ≤ 200ms）
#define TDMA_BEACON_US 10000     // 与 TdmaMac.cpp 常量一致
#define TDMA_DL_US 10000
#define TDMA_SLOT_US 10000
#define TDMA_REG_US 10000
#define TDMA_DEV_TIMEOUT_MS 30000

// ===== 信道质量检测（开机避开干扰频点，见 docs/tdma-mac.md §6）=====
#define GW_FREQ_QUAL_SAMPLES 50      // 每频点 RSSI 采样数（×间隔 = 每点 ~105ms，20 点 ~2.1s）
#define GW_FREQ_QUAL_INTERVAL_MS 2   // 采样间隔
#define GW_FREQ_AVG_THRESH_DBM -95   // 均值阈值：高于视为噪声底偏高（城市/干扰源环境调低）
#define GW_FREQ_MAX_THRESH_DBM -85   // 峰值阈值：高于视为有信号活动（同频 LoRa/强干扰）
#define GW_FREQ_SEARCH_WINDOW 4      // 避干扰时在理想频点 ±N 栅格（±N×2MHz）内搜索替代

// ===== 状态 LED =====
#define PIN_STATUS_LED 2         // 板载 LED

// ===== 显示屏（SSD1306 OLED 128x64 I2C）=====
#define PIN_OLED_SDA 21          // ESP32 I2C SDA
#define PIN_OLED_SCL 22          // ESP32 I2C SCL
#define OLED_ADDR 0x3C

// ===== 网关间组播通道（局域网直连，降低命中感知延时，见 docs/gateway-mesh.md）=====
#define MCAST_IP IPAddress(239, 255, 43, 21)
#define MCAST_PORT 43210
