#pragma once

// ===== 设备身份 =====
#define DEV_IDX 1                // 8 位短设备号（0~255，每把枪唯一；网关据此识别发送者）
#define DEVICE_ID "G0001"        // 每把枪唯一，网关/服务器据此区分
#define DEVICE_NAME "玩家A"       // 显示名，可在 J 帧中由 AT 配置覆盖
#define FW_VERSION "1.0.0"

// ===== 引脚定义（详见 docs/hardware-design.md 与 PCB/main-board-S3引脚映射.md）=====
// 依据：gun-board 主控页原理图（P1，2026-09-04 导出 enet/PDF）——
//   ✅=原理图已连线（图确认）；🆕=空脚建议分配（待按映射表布线）
// S3 约束：GPIO26-32=Flash、33-37=Octal PSRAM、19/20=USB、0/3/45/46=strapping。
// ⚠️ GPIO45/46 用于 SX BUSY/RST 是原理图确认连法（立创板排针引出，可用）；上电时序无碍。
#define PIN_IR_TX 13             // ✅ 940nm 远距 OOK →GPIO13（主控页 $1N92→220Ω→Q4 栅，H2 接 LED）
#define PIN_IR_TX_850 47         // ✅ 近距 940nm/56kHz OOK（主控页网络 '850nm'=G47→200Ω→Q1 栅）
#define PIN_IR_RX 4              // 🆕 远距接收 38kHz IRM：XL-IRM-V838M3/TR（C51900936；空闲高，中断）
#define PIN_IR_RX_850 5          // 🆕 近距 56kHz IRM 预留（56k 暂缓；空闲高，中断；历史命名 _850）
#define PIN_TRIGGER 6            // ✅ 扳机微开关（主控页 G06 悬空 stub 即此；上拉，按下接地）
#define PIN_MOTOR 7              // 🆕 水弹波箱电机 MOSFET 栅极（G07 空脚）
#define PIN_LED_DATA 8           // 🆕 WS2812 数据脚（G08 空脚）

// ===== 音频（MAX98357A I2S 功放 + 8Ω 小喇叭；替代蜂鸣器）=====
#define PIN_I2S_BCLK 9           // 🆕 MAX98357A BCLK（G09 空脚；S3 GPIO matrix 任意映射）
#define PIN_I2S_WS 12            // 🆕 MAX98357A LRCK（G12 空脚；原 18 已被 OLED SCL 占用）
#define PIN_I2S_DOUT 11          // 🆕 MAX98357A DIN（G11 空脚）

// ===== SX1268 470MHz LoRa（Ra-01S 模组 = 主控页 U2，SPI 直驱）=====
#define PIN_SX_NSS 16            // ✅ CS=G16（主控页 CS→U2.NSS）
#define PIN_SX_SCLK 42           // ✅ SCK=G42
#define PIN_SX_MOSI 15           // ✅ MOSI=G15
#define PIN_SX_MISO 41           // ✅ MISO=G41
#define PIN_SX_RST 46            // ✅ RaRES=G46（低有效复位）
#define PIN_SX_BUSY 45           // ✅ RaBUSY=G45（忙指示）
#define PIN_SX_DIO1 (-1)         // DIO1 不接（RadioLib 轮询模式；Module 传 -1）

#define RADIO_FREQ_MHZ 470.0f    // 470~510MHz 国内合法微功率频段
#define RADIO_BW_KHZ 500.0f      // 目标：SF7/500k（21.9kbps，见 docs/wireless-research.md）
#define RADIO_SF 7               // 扩频因子（距离/速率折中）
#define RADIO_CR 5               // 编码率 4/5
#define RADIO_SYNC_WORD 0x12     // 私有网络同步字（0x12=私有，0x34=公共LoRaWAN）
#define RADIO_TX_POWER_DBM 15    // 合规 17dBm e.r.p.（15dBm 传导 + 2dBi 天线）；实验档可调 22
#define RADIO_PREAMBLE_LEN 4     // 短前导：腾出时隙预算（10B 帧空口 ~9.3ms）

// ===== TDMA MAC（自适应时隙 + 开机自动分配信道，见 docs/tdma-mac.md）=====
#define TDMA_ENABLE 1            // 1=TDMA 时隙模式（替代 ALOHA 即时发送）
// 标准频点栅格（与网关自动分配一致，见 TdmaProto.h TDMA_STD_*）：
// 470.0 + k×2.0MHz，k=0..19。设备扫描全栅格 → 只听网关实际使用频点 → 兼容射频没装满。
// 快速单信道测试可临时改 1（仅扫 470.0）。
#define TDMA_CHANNELS TDMA_STD_CHANNELS
#define TDMA_MAX_SLOTS 17        // 每信道最大设备时隙（+1 注册时隙 ≤ 200ms）
#define TDMA_BEACON_US 10000     // 信标窗（与 TdmaMac.cpp 常量一致）
#define TDMA_DL_US 10000         // 广播下行窗
#define TDMA_SLOT_US 10000       // 设备时隙
#define TDMA_REG_US 10000        // 注册时隙
#define TDMA_DEV_TIMEOUT_MS 30000 // 设备心跳超时（网关移除并重排时隙）

// ===== 显示屏（SSD1306 OLED 128x64 I2C，软件 I2C 任意引脚）=====
#define PIN_OLED_SDA 17          // ✅ SDA=G17（主控页 → OLED1.SDA）
#define PIN_OLED_SCL 18          // ✅ SCL=G18（主控页 → OLED1.SCL）
#define OLED_ADDR 0x3C

// ===== 菜单按钮（上/下/确认/取消，按下接地；S3 全部用内部上拉）=====
#define PIN_BTN_UP 1             // 🆕 上（G01 空脚，内部上拉）
#define PIN_BTN_DOWN 2           // 🆕 下（G02 空脚，内部上拉）
#define PIN_BTN_OK 21            // 🆕 确认（G21 空脚，内部上拉；GPIO0 保留给板载 BOOT）
#define PIN_BTN_CANCEL 40        // 🆕 取消（G40 空脚，内部上拉）
#define MENU_TIMEOUT_MS 10000    // 菜单无操作超时，返回战斗界面

// ===== 红外编解码参数（NEC 风格，见 docs）=====
// 载波半周期（整数 µs）：
//   940 远距通道：13µs → 38.46kHz。接收头为 XL-IRM-V838M3/TR（成兴光，中心 37.9kHz，
//   带通容差 ±2~3kHz）→ 偏差 0.56kHz 在带通内 ✓，无需改；若实测灵敏度不足，可改
//   "13/14µs 交替翻转"（平均 37.9kHz）或直接用 13.2µs 级定时（需换定时器方案）。
//   850 近距通道：9µs → 55.6kHz（56kHz 接收头待定，56k 暂缓）。
#define IR_CARRIER_HALF_US 13    // 远距 940nm 通道载波半周期（38.46kHz ≈ 37.9k 带通内）
#define IR_CARRIER_HALF_US_850 9 // 近距 56kHz 通道载波半周期（55.6kHz；56k 接收头待定）
#define IR_PREAMBLE_US 9000      // 引导码 9ms
#define IR_PREAMBLE_GAP_US 4500  // 引导码后间隔 4.5ms
#define IR_BIT_HIGH_US 560       // 载波脉冲
#define IR_BIT_ONE_GAP_US 1690   // bit1 空闲
#define IR_BIT_ZERO_GAP_US 560   // bit0 空闲
#define IR_REPEAT_MS 140         // 连发重复间隔（48bit 帧 ~95ms，需 > 单帧时长）
#define IR_PULSE_TOL_US 300      // 脉宽容差
#define IR_GAP_THRESH_US 1125    // 0/1 判别阈值

// ===== 作用范围：功率档位与双帧 =====
// ✅ 主控页近距档位：Q2 栅=G39、Q3 栅=G38（网络 850nm-2/-3），基础支路 R182=82Ω 常通
#define PIN_IR_POWER 14          // 🆕 远距（940nm）功率档切换（G14 空脚，高=远档）；940 单档硬件可不接→0xFF
#define PIN_IR_PWR_850_A 39      // ✅ 近距功率档 bit0 = G39（Q2 栅；低=该支路断开）
#define PIN_IR_PWR_850_B 38      // ✅ 近距功率档 bit1 = G38（Q3 栅）
#define IR_PWR_850_LEVELS 4      // 近距通道档位数（2 或 4，文档/校准用；固件按引脚有效性自动判定）
#define IR_DUAL_FRAME 1          // 每扣扳机发 2 帧相同 shotSeq（提升远距离成功率）
#define IR_DUAL_FRAME_GAP_MS 20  // 双帧间隔（双通道并行后单帧 ~80ms，20ms 间隔保证帧界清晰）
#define DEFAULT_POWER_LEVEL 1    // 0..3（服务器 W 帧可覆盖；两通道独立映射，见 hardware-design §6.5）

// ===== 电机联动（非阻塞定时器，与红外发射并行，控制开火总延时 <200ms）=====
#define MOTOR_ON_MS 60           // 水弹波箱联动时长

// ===== 户外强光自适应 =====
#define PIN_LIGHT_SENSE 10       // 环境光检测（光敏二极管+分压 → ADC1_CH10，S3 ADC1=GPIO1-10）；0xFF=禁用
#define LIGHT_SAMPLE_MS 500      // 采样周期
#define LIGHT_AUTO_POWER 1       // 强光自动升远档（1=开 0=关）
#define LIGHT_HIGH_ADC 2800      // ADC 12bit 阈值：高于=强光（阳光直射）

// ===== 本地默认规则（收到服务器 W 帧后覆盖）=====
#define DEFAULT_HP 100
#define DEFAULT_AMMO 120
#define DEFAULT_RELOAD_MS 2000
#define DEFAULT_RESPAWN_MS 3000
#define DEFAULT_TEAM 0

// ===== 心跳 / 重连 =====
#define HEARTBEAT_MS 10000
#define REJOIN_MS 5000

// ===== 无改装联动（检测板 ESP-NOW，见 docs/trigger-sensor-study.md）=====
#define GUN_ESPNOW_ENABLE 1
#define TRIGGER_BOARD_MAC { 0x24, 0x0A, 0xC4, 0x00, 0x00, 0x02 }  // 检测板 MAC（与检测板 GUN_MAC 互填）
#define ESPNOW_CHANNEL 1          // 2.4G 信道（与检测板一致）

// ===== 日志上传 =====
#define LOG_UPLOAD_IDLE_MS 30000  // 空闲定时上传间隔（有待传时）
#define LOG_UPLOAD_FULL 0.75f     // 缓冲水位阈值：超过则立即上传
#define LOG_PAYLOAD_MAX 48        // 单分片 payload 最大长度（L 帧 ≤64B）

// ===== 光强度检测（同通道仲裁用；双帧窗口计数为光强代理，可升级 ADC）=====
#define DEATH_DELAY_MS 30         // D 帧上报延迟：等待双帧窗口统计光强
#define ENERGY_STRONG 200         // 双帧都收到（强光）
#define ENERGY_WEAK 100           // 仅一帧（弱光）
