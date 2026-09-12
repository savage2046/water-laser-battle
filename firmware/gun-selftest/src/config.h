#pragma once

// ============================================================================
// gun-selftest 配置
//
// 板卡：gun-board（枪端主控板），主控 LCKFB-ESP32S3R8N8（ESP32-S3，DIP-40，
//       丝印 Gxx = GPIOxx）；射频 U2 Ra-01S/SX1268。
//
// ⚠️ 引脚/射频参数必须与正式固件 firmware/gun/src/config.h 一致。
// ============================================================================

// ===== 设备身份（枪端）=====
#define DEV_IDX 1                 // 8 位短设备号（与正式固件一致，网关据此识别）
#define DEVICE_ID "G0001"         // 5 字符 ASCII，随 JOIN 帧上报（网关学习 devIdx↔ID）
#define FW_VERSION "0.4.0"

// ===== 串口 =====
#define SERIAL_BAUD 115200
#define HB_MS 1000                // 串口心跳行周期

// ===== 板载 LED（灌电流：阳极→3.3V，阴极→限流电阻→GPIO48；低电平点亮）=====
#define PIN_LED 48
#define LED_FAIL_HALF_MS 120      // 射频初始化失败：快闪
#define LED_SCANNING_HALF_MS 260  // 射频 OK 但未连上网关：中速闪
#define LED_ONLINE_HALF_MS 1000   // 已连上网关（获时隙分配）：慢闪

// ===== SX1268 470MHz LoRa（U2 Ra-01S，SPI 直驱）=====
#define PIN_SX_NSS 16             // CS=G16
#define PIN_SX_SCLK 42            // SCK=G42
#define PIN_SX_MOSI 15            // MOSI=G15
#define PIN_SX_MISO 41            // MISO=G41
#define PIN_SX_RST 46             // RaRES=G46（低有效）
#define PIN_SX_BUSY 45            // RaBUSY=G45
#define PIN_SX_DIO1 4             // ✅ DIO1=G04（2026-09-10 已接线）：中断输出
                                  //    收包上升沿 → ISR 记 micros() → 到达时刻 ±µs，
                                  //    信标相位/时隙对齐偏移都以此为准（见 LoRaDev.cpp）
                                  //    未接线时写 -1：自动退回"轮询时间戳"

#define RADIO_FREQ_MHZ 470.0f
#define RADIO_BW_KHZ 500.0f
#define RADIO_SF 7
#define RADIO_CR 5
#define RADIO_SYNC_WORD 0x12
#define RADIO_TX_POWER_DBM 15
#define RADIO_PREAMBLE_LEN 4

// ===== TDMA 时隙常量（必须与 firmware/lib/tdma/TdmaMac.cpp 一致）=====
#define TDMA_BEACON_US 10000
#define TDMA_DL_US 10000
#define TDMA_SLOT_US 10000
#define TDMA_REG_US 10000
#define TDMA_AIR_US 9300          // 10B 帧 @SF7/BW500k/前导4 空口时长估算
#define TDMA_MAX_SLOTS 17
#define TDMA_STD_CHANNELS 20      // 470.0 + k×2.0 MHz

// ===== LoRa 网关自动连接 / 心跳 =====
#define GW_SCAN_DWELL_MS 250      // 每个频点听信标的驻留时间（20 点 ≈ 5s 一轮）
#define GW_BEACON_LOST_MS 5000    // 这么久没听到信标 → 判定掉线，重新扫描
#define HB_PERIOD_MS 1000         // ★ 每秒给网关发一次心跳（TF_HB）
#define HB_QUEUE_MAX 3            // 待发心跳队列深度（发不出去时丢旧的）
#define JOIN_EVERY_SF 2           // 每 N 个超帧发一次 JOIN（未获分配时）
#define JOIN_JITTER_US 2000       // JOIN 在注册槽内的随机退避（需 < 注册窗裕量-空口）
#define HB_LATE_WARN_US 2000      // 心跳晚于时隙起点这么多 → 记为"迟到"
// 网关实际超帧周期比标称 30+10N 多出的部分（lora-gwtest 默认 = 注册窗裕量 5ms + 让出 1ms）。
// 只用于「没收到信标时兜底推进本地相位」，避免心跳被卡住；收到信标时一律以信标为准。
#define GW_SF_EXTRA_US 6000

// ===== T3：ESP-NOW（2.4G，与检测板 / espnow-verify 一致）=====
#define ESPNOW_CHANNEL 1          // 2.4G 信道（须与对端一致；EspNowAuto 内部同名 ES_CHANNEL）

// ===== 940nm / 38kHz 光路矫正发射（每秒亮 0.7s，最低功率）=====
// 近距通道（历史命名 850nm）：数据 G47、功率档 A=G39/B=G38
// 远距通道：数据 G13（H2 排针 → 940 LED，带准直透镜）、功率档 G14（940 单档硬件，未用）
#define PIN_IR_TX_FAR 13          // 远距通道 OOK 输出
#define PIN_IR_TX_NEAR 47         // 近距通道 OOK 输出
#define PIN_IR_PWR_NEAR_A 39      // 近距功率档 bit0（Q2 栅；高=该支路导通）
#define PIN_IR_PWR_NEAR_B 38      // 近距功率档 bit1（Q3 栅）
#define PIN_IR_POWER_FAR 14       // 远距功率档（单档硬件 → 0xFF 表示不使用）
// 0xFF = 未接线/不使用
#define IR_ALIGN_CH_BOTH 2
#define IR_ALIGN_CH 2             // 0=仅近距(G47) 1=仅远距(G13) 2=两个都发（默认）
#define IR_ALIGN_PERIOD_MS 1000   // 每秒一个周期
#define IR_ALIGN_ON_MS 700        // ★ 光信号持续 0.7s（周期内亮 0.7s / 灭 0.3s）
                                  //   实现：0.7s 内**背靠背连发编码帧**（每帧 ~81ms → 约 8 帧），
                                  //   既"常亮可见"便于目视对准，又能被接收端反复解码验证
#define IR_ALIGN_HALF_US 13       // 载波半周期 → 38.46kHz（XL-IRM-V838M3 中心 37.9k 带通内）
#define IR_ALIGN_POWER_LEVEL 0    // ★ 最低功率：近距 00=0.5×I_nom；远距档位 0/1 均为近档
#define IR_ALIGN_WEAPON 0         // 帧内 weaponId（0=步枪）
#define IR_ALIGN_TEAM 3           // 帧内 team（3=无队伍）
#define IR_ALIGN_USE_DEV_IDX 1    // 1=playerId 用 DEV_IDX（被接收端当成真实命中，便于判读）

// ===== 红外帧编码常量（与 firmware/gun/src/LaserCodec.cpp 完全一致，勿改）=====
#define IR_PREAMBLE_US 9000       // 引导码 9ms 载波
#define IR_PREAMBLE_GAP_US 4500   // 引导码后 4.5ms 间隔
#define IR_BIT_HIGH_US 560        // 每位起始载波脉冲
#define IR_BIT_ONE_GAP_US 1690    // bit1 空闲
#define IR_BIT_ZERO_GAP_US 560    // bit0 空闲
