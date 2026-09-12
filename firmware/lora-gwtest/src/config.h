#pragma once

// ============================================================================
// lora-gwtest 配置
//
// 板卡：gun-board（枪端主控板，"另一块枪端 PCB" 充当 LoRa 网关）
//       主控 LCKFB-ESP32S3R8N8（ESP32-S3，DIP-40，丝印 Gxx = GPIOxx）
//
// ⚠️ 本文件所有引脚/射频参数必须与 firmware/gun/src/config.h、
//    firmware/gateway/src/config.h 保持一致——不一致就测不出正式链路的真实表现。
// ============================================================================

#define FW_VERSION "0.1.0"

// ===== 板载 LED（灌电流：阳极→3.3V，阴极→限流电阻→GPIO48，低电平点亮）=====
#define PIN_LED 48

// ===== SX1268 470MHz LoRa（U2 Ra-01S，SPI 直驱；主控页 ✅ 已连线）=====
#define PIN_SX_NSS 16            // CS=G16
#define PIN_SX_SCLK 42           // SCK=G42
#define PIN_SX_MOSI 15           // MOSI=G15
#define PIN_SX_MISO 41           // MISO=G41
#define PIN_SX_RST 46            // RaRES=G46（低有效）
#define PIN_SX_BUSY 45           // RaBUSY=G45
#define PIN_SX_DIO1 4            // ✅ DIO1=G04（2026-09-10 已接线）：中断输出
                                 //    ① 收包上升沿 → ISR 记 micros() → 到达时刻精确到 ±µs
                                 //       （本固件的信标相位/时隙偏移以此为准）
                                 //    ② 未接线时须写 -1：本固件会自动退回"轮询时间戳"

// ===== 射频参数（与枪/盔/网关三端完全一致）=====
#define RADIO_FREQ_MHZ 470.0f    // 栅格 k=0（实际频点由栅格索引决定）
#define RADIO_BW_KHZ 500.0f
#define RADIO_SF 7
#define RADIO_CR 5
#define RADIO_SYNC_WORD 0x12
#define RADIO_TX_POWER_DBM 15    // 合规 17dBm e.r.p.
#define RADIO_PREAMBLE_LEN 4

// ===== TDMA 时隙常量（必须与 firmware/lib/tdma/TdmaMac.cpp 顶部常量一致）=====
// 超帧 = 信标窗 10 + 下行窗 10 + N×设备时隙 10 + 注册时隙 10 ms（N=1..17）
#define TDMA_BEACON_US 10000
#define TDMA_DL_US 10000
#define TDMA_SLOT_US 10000
#define TDMA_REG_US 10000
#define TDMA_AIR_US 9300         // 10B 帧 @SF7/BW500k/前导4 空口时长估算
#define TDMA_MAX_SLOTS 17        // 每信道最大设备时隙
#define TDMA_DEV_TIMEOUT_MS 30000  // 设备心跳超时（与 TdmaMac 一致）

// 接收窗裕量（仅本测试固件用；正式 TdmaMac 固定 +2000us，见 README §6 的差异说明）
//   时隙窗：帧固定在时隙起点 +9.3ms 结束，忙等轮询下 +0.5ms 裕量已足够
//   注册窗：设备 tryJoin 带 0-6ms 随机退避，JOIN 最早 regStart、最晚 regStart+6ms 才起发，
//           空口 9.3ms → 结束最晚 regStart+15.3ms。裕量必须 ≥5.3ms 才能收全（+5ms→95%）
//   注意：本固件的等效超帧周期 = 30+10N + GW_REG_RX_EXTRA_US + 1ms（恒定），
//        设备端每收到一个信标就重新锁相，因此不会失锁（见 LoRaTest::runGw 注释）
#define GW_SLOT_RX_EXTRA_US 500
#define GW_REG_RX_EXTRA_US 5000

// ===== 频点扫描 =====
#define SCAN_SAMPLES 24              // 每频点瞬时 RSSI 采样数
#define SCAN_INTERVAL_MS 2           // 采样间隔（24×2=48ms/点）
#define SCAN_DWELL_MS 300            // 采样后继续驻留收帧的时间
#define SCAN_NOISY_AVG_DBM (-95)     // 底噪均值阈值（高于视为噪声偏高）
#define SCAN_NOISY_MAX_DBM (-85)     // 峰值阈值（高于视为有信号活动）
#define SCAN_AUTO_MON 1              // 扫完自动锁定到占用最强的频点并转 MON
#define SCAN_IDLE_MS 1000            // 无占用时 SCAN 待命轮询间隔

// ===== 测试帧类型（0x14~0x1F 仓库未使用；正式固件 default: break 忽略）=====
#define TF_TEST_PING 0x1E
#define TF_TEST_PONG 0x1F
#define PING_TARGET_IDX 0xFF         // 广播 ping（所有 ECHO 应答器都会回）
#define ECHO_DEV_IDX 0xFE            // 本板作为 ECHO 应答器时的 devIdx（非真实设备号）

// ===== Ping-Pong 参数 =====
#define PING_PERIOD_MS 500           // 持续 ping 周期
#define PING_TIMEOUT_MS 1200         // 单次 ping 未回判定丢失的时间
#define PING_BUSY_US 60000           // 发完 ping 后的忙等（高精度）接收窗口
#define PING_BATCH_DEFAULT 20        // 串口 p 命令默认发多少个
#define PING_ECHO_DELAY_MS 0         // ECHO 应答延迟（0=立即回）

// ===== 统计规模 =====
#define MAX_DEVS 32                  // 每设备统计表项（单信道 ≤17 设备，32 足够）
#define MAX_RTT_SAMPLES 256          // RTT 百分位样本数（P95）

// ===== 串口 =====
#define SERIAL_BAUD 115200
#define HB_MS 5000                   // 心跳周期
#define FRAME_LOG_DEFAULT 1          // 开机是否逐帧打印 #rx 行

// ===== 事件环形缓冲（射频任务 → 打印任务）=====
#define EVT_RING 48
#define NOTE_RING 12
#define NOTE_LEN 96
