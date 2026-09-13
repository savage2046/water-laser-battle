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

// ===== 联调开关：先跑通 LoRa，暂时整体关掉 WiFi/服务器（2026-09-13）=====
// 背景：WiFi 段在 setup() 里是**阻塞等连上**
//       （while (WiFi.status() != WL_CONNECTED) { delay(500); }），
//       而 SSID / 服务器地址还是占位值（"your-wifi" / 192.168.1.100）
//       → 会一直卡在开机阶段，后面 loop() 的心跳行也永远不出来。
//       联调 LoRa 完全用不到 WiFi，故先整体关掉。
//   0 = 不初始化 WiFi、不连 WebSocket、不发网关间组播；LoRa / TDMA / 串口全部照常。
//       本该上报服务器的 JSON 会**镜像到串口**（见下一行的开关）——不接服务器也能看见
//       JOIN / ASSIGN / 心跳 / 击杀等上行内容。
//   1 = 恢复完整功能（记得先把 WIFI_SSID / WIFI_PASS / SERVER_HOST 填成真实值）。
#define GW_WIFI_ENABLE 0
#define GW_MIRROR_UPLINK_SERIAL 1   // WiFi 关闭时把上行 JSON 打到串口（0 = 不打）

// ===== SX1268 470MHz LoRa（SPI 直驱）=====
// ⚠️ 2026-09-12 网关板由「ESP32 经典款」换成 **ESP32-S3**，并**改用与枪端主板完全相同的接线**
//    （见 firmware/gun/src/config.h）→ 本文件引脚与枪端逐项一致：
//      NSS=G16  SCLK=G42  MOSI=G15  MISO=G41  RST=G46  BUSY=G45  DIO1=G04
//    这样两端的板子/线束完全一样，烧同一个测试固件（gun-s3 env）就能互通。
//    S3 约束核对：G41/G42 = JTAG（可用，接外部调试器时才冲突）、G45/G46 = strapping 脚
//    （上电瞬间被采样，本板实测无碍，见 PCB 引脚映射表）、G26~G32 Flash / G33~G37 PSRAM /
//    G19/G20 USB —— 均未占用。开机自检会再核一遍（gateway/main.cpp 的 checkPinsForS3）。
#define PIN_SX_NSS 16            // SPI 片选（与枪端一致）
#define PIN_SX_SCLK 42           // SPI 时钟
#define PIN_SX_MOSI 15           // SPI 主机输出
#define PIN_SX_MISO 41           // SPI 主机输入
#define PIN_SX_RST 46            // 复位
#define PIN_SX_BUSY 45           // 忙指示
#define PIN_SX_DIO1 4            // SX1268 DIO1 中断（已接线到 G04）
                                 // ⚠️ 只有**第一个/唯一一个**射频（槽 0）能用它：
                                 //    多射频板每个射频都要一根 DIO1，引脚不够 →
                                 //    kRfSlots 里其余槽位 DIO1 填 -1。
                                 //    TdmaMac 收发已改轮询 IRQ 寄存器（不依赖 DIO1），
                                 //    所以接上与否不影响 TDMA 功能（见 README/文档 §4.1）

#define RADIO_FREQ_MHZ 470.0f    // 全队一致：470.0MHz
#define RADIO_BW_KHZ 500.0f      // 目标：SF7/500k（见 docs/wireless-research.md）
#define RADIO_SF 7
#define RADIO_CR 5
#define RADIO_SYNC_WORD 0x12     // 私有网络同步字（0x12=私有，0x34=公共LoRaWAN）
#define RADIO_TX_POWER_DBM 15    // 合规 17dBm e.r.p.（15dBm 传导 + 2dBi 天线）
#define RADIO_PREAMBLE_LEN 4     // 短前导：腾出时隙预算

// ===== TDMA MAC（自适应时隙 + 自动分配信道，见 docs/tdma-mac.md）=====
#define TDMA_ENABLE 1
// 射频槽位数（开机自检：实际装几个用几个，没装满自动兼容）。
//
// 容量目标（2026-09-13 修订，原「200 台 / 12 信道」方案作废）：
//   **50 台同时在线** = 20 人 × 2 台（枪 + 头盔）= 40 台，再预留 10 台。
//   单信道 = TDMA_MAX_SLOTS 17 台 → **3 信道 = 51 台 ≥ 50 ✓**（超帧仍 ≤200ms）。
//   → 目标硬件：**3 颗 SX1268**，每颗一个信道（理由见 docs/wireless-research.md §4.1/§7）。
//   槽位表留 4 槽 = 3 主用 + 1 扩展余量；槽 1..3 的引脚待多射频板 PCB 定稿后填。
//   副产品：3 信道时间隔 18~20MHz（原 12 信道勉强 ≥2MHz），
//   15dBm 发射对邻道模块的阻塞风险大幅下降。
#define GW_RF_SLOTS 4
#define TDMA_MAX_SLOTS 17        // 每信道最大设备时隙（+1 注册时隙 ≤ 200ms）；×3 信道 = 51 台
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

// ===== 状态灯（G48 模组板载 LED，普通单色灯、灌电流接法）=====
// ⚠️ 2026-09-13 与枪端统一：灯在 **G48**，是模组板上自带的**普通单色 LED**
//    （阳极→3.3V、阴极→限流电阻→GPIO48 → **低电平点亮**，载板无需接线；
//     依据 PCB/gun-board/main-ESP32主控-U1管脚定义-在线读取.md）。
//    原来是 `PIN_STATUS_LED 2` —— 经典 ESP32 的板载 LED 脚，在 S3 上 G2 是空脚
//    （枪端把 G2 当按键用）→ 这颗灯从来没亮过。
//    ⚠️ 是**单色灯不是 WS2812** → 用普通 digitalWrite（注意极性反了），不要用 FastLED。
#define PIN_LED 48               // ✅ 与枪端一致（模组板载 LED，G48）
#define LED_ON_LEVEL LOW         // 灌电流接法：**低电平点亮**

// ===== 显示屏（SSD1306 OLED 128x64 I2C）=====
// ⚠️ 2026-09-13 改为**与枪端完全相同的接线**（同 SX1268 那次统一的做法）。
//    原来是 21/22 —— 那是**经典 ESP32 的默认 I2C 脚**，而 ESP32-S3 **根本没有 GPIO22**
//    （S3 的 GPIO 只有 0~21、26~48，22~25 不存在）→ 屏在 S3 上必然点不亮。
//    枪端用的是 17/18（已验证可用），两端线束一致后才能烧同一个固件对照。
#define PIN_OLED_SDA 17          // ✅ SDA=G17（与枪端一致）
#define PIN_OLED_SCL 18          // ✅ SCL=G18（与枪端一致）
#define OLED_ADDR 0x3C

// ===== 网关间组播通道（局域网直连，降低命中感知延时，见 docs/gateway-mesh.md）=====
#define MCAST_IP IPAddress(239, 255, 43, 21)
#define MCAST_PORT 43210