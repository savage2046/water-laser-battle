#pragma once

// ============================================================
// calibration-target 校准靶（按 PCB/calibration-target/calibration-target.eprj2 原理图 2026-09-04）
//   - 15× 38kHz/940nm IRM：XL-IRM-V838M3/TR（成兴光 C51900936），3.3V 供电，TTL 低有效
//   - 15× WS2812C-2020-V6（C55109522）RGB 指示：5V 供电，单线串（800kHz），经 74HCT1G125 电平转换
//   - 主控：立创 LCKFB-ESP32S3R8N8 开发板（DIP-40，丝印 Gxx = GPIOxx）
//   - 行为：某格 IRM 收到 940nm(38k) 载波 → 对应 LED 红；未收到 → 绿
//   灯串链序（物理逐颗串接）：U11→U12→…→U20→U41→U42→…→U45
//   链序 LED0..14 与 IRM1..15 一一对应同格（见 README 位置图）
// ============================================================

// ===== 阵列 =====
#define CELL_COUNT 15            // 接收格数（IRM 数 = LED 数）
#define IRM_ACTIVE_LOW 1         // XL-IRM 收到载波时 OUT=低

// 格 n(0..14) 对应 IRM(n+1)，引脚顺序与原理图 IRM1..IRM15 网络一致：
//   IRM1..15 → GPIO 1,2,42,4,5,6,7,8,9,10,11,12,13,14,15
//   注：GPIO3 为 strapping 脚，原理图把 IRM3 改走 GPIO42；GPIO0/19/20/26-37/45/46 未用
#define IRM_GPIO_1   1
#define IRM_GPIO_2   2
#define IRM_GPIO_3   42   // 原 3 号被 strapping 占用 → 原理图用 GPIO42
#define IRM_GPIO_4   4
#define IRM_GPIO_5   5
#define IRM_GPIO_6   6
#define IRM_GPIO_7   7
#define IRM_GPIO_8   8
#define IRM_GPIO_9   9
#define IRM_GPIO_10  10
#define IRM_GPIO_11  11
#define IRM_GPIO_12  12
#define IRM_GPIO_13  13
#define IRM_GPIO_14  14
#define IRM_GPIO_15  15

// ===== 灯串（WS2812C-2020-V6，5V）=====
#define PIN_LED_DATA 41          // U32.DIP37=G41 → U33 74HCT1G125.A；Y → U11.DIN（链首）
#define LED_COUNT    15          // 链序 U11..U20, U41..U45
#define LED_TYPE     NEOPIXEL    // 800kHz 单线协议（WS2812C 兼容）

// 颜色（FastLED CRGB）
#define LED_IDLE_COLOR CRGB(0, 90, 0)    // 未收到 → 绿（按需调暗）
#define LED_HIT_COLOR  CRGB(255, 0, 0)    // 收到 940nm → 红

// ===== 检测/显示参数 =====
#define IRM_MIN_BURST_US 150     // 一次有效载波突发最短时长(µs)，滤毛刺（38k≈26µs/周期，>5 周期）
#define LED_HIT_HOLD_MS  600    // 命中保持：最后一次有效突发后 LED 保持红的时间
#define LED_REFRESH_MS   25     // 灯串刷新周期
#define FASTLED_BRIGHTNESS 110  // 全局亮度 0-255

// ===== U8（IRM8 = 格 idx7 = GPIO8）额外完整解码 =====
// U8 除"有 38k 就红"外，还解码完整 40bit 子弹帧（与枪端 NEC 风格帧一致，FrameRx）；
// 解出并通过校验后，对应格 LED 蓝色闪烁 3 次，串口打印全部解码信息
#define DECODE_CELL_IDX 7            // U8 = IRM8 → 灯链索引 7（LED idx = IRM 号-1）
#define IRM_GPIO_DECODE IRM_GPIO_8   // U8.OUT = GPIO8（FrameRx 通道 0，独占 CHANGE 中断）
#define LED_DECODE_COLOR CRGB(0, 0, 255)  // 解码成功闪烁色：蓝
#define DECODE_FLASH_ON_MS  150      // 蓝闪点亮时长
#define DECODE_FLASH_OFF_MS 150      // 蓝闪熄灭时长（期间回落到红/绿）
#define DECODE_FLASH_TIMES  3        // 蓝闪次数

// 红外帧时序（与枪端 firmware/gun 一致，FrameRx 依赖；单位 µs）
#define IR_PREAMBLE_US 9000
#define IR_PREAMBLE_GAP_US 4500
#define IR_BIT_HIGH_US 560
#define IR_BIT_ONE_GAP_US 1690
#define IR_BIT_ZERO_GAP_US 560
#define IR_PULSE_TOL_US 300
#define IR_GAP_THRESH_US 1125

// ===== 开机扫灯自检（逐颗点亮核对位置）=====
#define SELFTEST_ENABLE 1          // 1=开机逐颗扫灯
#define SELFTEST_STEP_MS 300       // 每颗点亮停留时长
#define SELFTEST_COLOR CRGB(255, 255, 255)  // 扫灯显示色：白（最亮易辨）

// 串口调试：1=上电打印映射与命中掩码；解码成功后打印完整子弹信息
#define SERIAL_DEBUG 1
