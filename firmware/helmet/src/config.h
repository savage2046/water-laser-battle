#pragma once

// ===== 头盔身份 =====
#define DEV_IDX 9                // 8 位短设备号（与枪端不重复；网关据此识别发送者）
#define DEVICE_ID "H0001"        // 头盔唯一 ID
#define MASTER_DEVICE_ID "G0001" // 所属枪的 deviceId（服务器据此转发命中给主人的枪）
#define DEVICE_NAME "玩家A头盔"
#define FW_VERSION "1.0.0"

// ===== 红外接收（双载波 38/56kHz × 前后方位 = 4 路，波长统一 940nm，360° 覆盖）=====
#define PIN_IR_RX0 26            // 前路 38kHz IRM：XL-IRM-V838M3/TR（成兴光，940nm 峰值，中断引脚）
#define PIN_IR_RX1 25            // 后路 38kHz IRM：XL-IRM-V838M3/TR（成兴光，940nm 峰值，中断引脚）
#define PIN_IR_RX850_0 36        // 前路 56kHz IRM（940nm 峰值，SMD 首选，纯输入；历史命名 _850）
#define PIN_IR_RX850_1 39        // 后路 56kHz IRM（940nm 峰值，SMD 首选，纯输入；历史命名 _850）
#define IR_RX_COUNT 4

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

// ===== TDMA MAC（自适应时隙 + 开机自动分配信道，见 docs/tdma-mac.md）=====
#define TDMA_ENABLE 1
// 标准频点栅格（与网关自动分配一致，见 TdmaProto.h TDMA_STD_*）：
// 470.0 + k×2.0MHz，k=0..19。设备扫描全栅格 → 只听网关实际使用频点 → 兼容射频没装满。
// 快速单信道测试可临时改 1（仅扫 470.0）。
#define TDMA_CHANNELS TDMA_STD_CHANNELS
#define TDMA_MAX_SLOTS 17        // 每信道最大设备时隙（+1 注册时隙 ≤ 200ms）
#define TDMA_BEACON_US 10000     // 与 TdmaMac.cpp 常量一致
#define TDMA_DL_US 10000
#define TDMA_SLOT_US 10000
#define TDMA_REG_US 10000
#define TDMA_DEV_TIMEOUT_MS 30000

// ===== 音频（MAX98357A I2S 功放 + 8Ω 小喇叭；替代蜂鸣器与 OLED）=====
#define PIN_I2S_BCLK 21          // 原 OLED SCL 释放
#define PIN_I2S_WS 22            // 原 OLED SDA 释放
#define PIN_I2S_DOUT 33          // 原蜂鸣器脚复用

// ===== 多色灯（WS2812，头顶状态灯）=====
#define PIN_LED 32
#define LED_COUNT 1

// ===== GPS 模块（UART1 单向接收 NMEA；头盔在头顶，信号优于枪身）=====
#define PIN_GPS_RX 34            // 接 GPS 模块 TX（纯输入引脚，单向接收）
#define GPS_BAUD 9600
#define GPS_REPORT_THRESH_M 8    // 移动超过 8 米才上报（省 915M 带宽）
#define GPS_REPORT_INTERVAL_MS 30000  // 无移动也每 30s 上报一次

// ===== GPS 省电（电源开关 + 热启动 + 运动唤醒）=====
#define PIN_GPS_PWR 13           // MOSFET 控制 GPS VCC（AO3401 P-MOS，低电平导通）；0xFF=禁用省电
#define GPS_FIX_TIMEOUT_MS 3000  // 上电后等 fix 超时（热启动 ~1s）
#define GPS_NOFIX_RETRY_MS 60000 // 无 fix 断电后重试间隔
#define GPS_POWER_CYCLE_MS 30000 // 无运动传感器时的兜底周期（有运动唤醒则静止不唤醒）
#define GPS_STILL_SLEEP_MS 5000  // 定位上报后静止冷却：此期间忽略运动中断（防抖动频繁唤醒）

// ===== 运动传感器（LIS3DH，运动唤醒 GPS）=====
#define PIN_MOTION_SDA 27        // 软件 I2C SDA（与 OLED 分线）
#define PIN_MOTION_SCL 14        // 软件 I2C SCL
#define PIN_MOTION_INT 15        // LIS3DH INT1 运动中断（ESP32 中断引脚）

// ===== 9 轴 IMU（MPU9250，记录头盔朝向供回放）=====
#define PIN_IMU_SDA 27           // 与 LIS3DH 共享软件 I2C 总线（地址不同）
#define PIN_IMU_SCL 14
#define PIN_IMU_PWR 12           // MPU9250 VCC 电源开关（AO3401，低电平导通；GPIO12 需下拉 10kΩ）

// ===== 红外编解码参数（与枪端一致，NEC 风格）=====
#define IR_PREAMBLE_US 9000      // 引导码 9ms
#define IR_PREAMBLE_GAP_US 4500  // 引导码后间隔 4.5ms
#define IR_BIT_HIGH_US 560       // 载波脉冲
#define IR_BIT_ONE_GAP_US 1690   // bit1 空闲
#define IR_BIT_ZERO_GAP_US 560   // bit0 空闲
#define IR_PULSE_TOL_US 300      // 脉宽容差
#define IR_GAP_THRESH_US 1125    // 0/1 判别阈值

// ===== 心跳 / 重连 =====
#define HEARTBEAT_MS 10000
#define REJOIN_MS 30000           // J 帧重发间隔：保持网关 devIdx 映射新鲜
