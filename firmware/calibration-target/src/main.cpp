#include <Arduino.h>
#include <FastLED.h>
#include "config.h"
#include "FrameRx.h"

// ============================================================
// calibration-target：15 路 940nm(38k) 接收 → 每格 WS2812C 红/绿指示
//   格 n 的 IRM 收到 940nm 载波 → LED[n] 红；否则 LED[n] 绿
//   U8（格 7 = IRM8 = GPIO8）额外做完整 40bit 帧解码：
//     解出完整子弹编码 → 该格蓝闪 3 次 + 串口输出全部解码信息
// ============================================================

// 灯串缓冲（链序 U11..U20, U41..U45 == IRM1..IRM15 同格）
static CRGB s_leds[CELL_COUNT];

// 每格 IRM OUT 引脚（索引 n 对应 IRM n+1）
static const uint8_t s_pins[CELL_COUNT] = {
  IRM_GPIO_1, IRM_GPIO_2, IRM_GPIO_3, IRM_GPIO_4, IRM_GPIO_5,
  IRM_GPIO_6, IRM_GPIO_7, IRM_GPIO_8, IRM_GPIO_9, IRM_GPIO_10,
  IRM_GPIO_11, IRM_GPIO_12, IRM_GPIO_13, IRM_GPIO_14, IRM_GPIO_15,
};

// 每格灯串器件位号（链序 U11..U20, U41..U45，仅用于开机扫灯/串口提示）
static const char *const s_ledRef[CELL_COUNT] = {
  "U11", "U12", "U13", "U14", "U15", "U16", "U17", "U18",
  "U19", "U20", "U41", "U42", "U43", "U44", "U45",
};

// ---------- 非解码格：CHANGE 中断量测有效载波突发 ----------
static volatile uint32_t s_loStartUs[CELL_COUNT];  // 本格 OUT 最近一次下降沿时刻
static volatile uint32_t s_lastHitUs[CELL_COUNT];  // 本格最近一次“有效突发”结束时刻

#define IRM_ISR(n)                                                             \
  static void IRAM_ATTR irmIsr##n() {                                          \
    uint32_t now = micros();                                                   \
    if (digitalRead(s_pins[n])) { /* 上升沿：低->高，突发结束 */               \
      if ((int32_t)(now - s_loStartUs[n]) >= (int32_t)IRM_MIN_BURST_US) {      \
        s_lastHitUs[n] = now;                                                  \
      }                                                                        \
    } else { /* 下降沿：高->低，突发开始 */                                    \
      s_loStartUs[n] = now;                                                    \
    }                                                                          \
  }
IRM_ISR(0) IRM_ISR(1) IRM_ISR(2) IRM_ISR(3) IRM_ISR(4)
IRM_ISR(5) IRM_ISR(6) IRM_ISR(7) IRM_ISR(8) IRM_ISR(9)
IRM_ISR(10) IRM_ISR(11) IRM_ISR(12) IRM_ISR(13) IRM_ISR(14)

#define IRM_ATTACH(n) \
  attachInterrupt(digitalPinToInterrupt(s_pins[n]), irmIsr##n, CHANGE)

// ---------- U8 解码格状态 ----------
static volatile uint32_t s_hit7AtMs = 0;  // U8 最近"有 38k 活动"时刻（frameRx.act[0] 消费）
static bool s_flashActive = false;        // 蓝闪进行中
static uint32_t s_flashStartMs = 0;
static uint16_t s_lastShotSeq = 0xFFFF;   // 去重（同 player+shotSeq 一枪只报一次）
static uint16_t s_lastPlayerId = 0xFFFF;
static uint32_t s_lastShotAtMs = 0;

static bool cellHit(uint8_t i) {
  uint32_t now = micros();
  return (int32_t)(now - s_lastHitUs[i]) < (int32_t)(LED_HIT_HOLD_MS * 1000L);
}

// 连续照射保持（非解码格）：某格 OUT 持续为低（>= 最短突发）视为仍在接收，持续刷新命中
static void maintainHits() {
  uint32_t now = micros();
  for (uint8_t i = 0; i < CELL_COUNT; i++) {
    if (i == DECODE_CELL_IDX) continue;  // 解码格由 act 活动标志驱动，不在此处理
    if (digitalRead(s_pins[i]) == LOW) {
      if ((int32_t)(now - s_loStartUs[i]) >= (int32_t)IRM_MIN_BURST_US) {
        s_lastHitUs[i] = now;
      }
    }
  }
}

static void startDecodeFlash() {
  s_flashActive = true;
  s_flashStartMs = millis();
}

// U8 蓝闪周期是否处于"点亮"相位（返回 true=显示蓝）
static bool decodeFlashOn() {
  if (!s_flashActive) return false;
  uint32_t e = millis() - s_flashStartMs;
  uint32_t total = DECODE_FLASH_TIMES * (DECODE_FLASH_ON_MS + DECODE_FLASH_OFF_MS);
  if (e >= total) {
    s_flashActive = false;
    return false;
  }
  return (e % (DECODE_FLASH_ON_MS + DECODE_FLASH_OFF_MS)) < DECODE_FLASH_ON_MS;
}

static void updateLeds() {
  maintainHits();

  // 解码格“有 38k 活动”→ 与其它格一致的红（600ms 保持）
  if (frameRx.act[0]) {
    frameRx.act[0] = false;
    s_hit7AtMs = millis();
  }
  bool hit7 = (millis() - s_hit7AtMs) < LED_HIT_HOLD_MS;

  bool flashOn = decodeFlashOn();
  for (uint8_t i = 0; i < CELL_COUNT; i++) {
    bool hit = (i == DECODE_CELL_IDX) ? hit7 : cellHit(i);
    CRGB c = hit ? LED_HIT_COLOR : LED_IDLE_COLOR;
    if (i == DECODE_CELL_IDX && flashOn) c = LED_DECODE_COLOR;  // 蓝闪覆盖
    s_leds[i] = c;
  }
  FastLED.show();
}

// U8 解码成功 → 蓝闪 + 串口完整输出
static void onFrameDecoded(const LaserFrame &f, uint64_t raw) {
  uint32_t now = millis();
  // 去重：同一枪（playerId+shotSeq）只报一次/只闪一次
  bool dup = (f.playerId == s_lastPlayerId && f.shotSeq == s_lastShotSeq &&
              now - s_lastShotAtMs < 1500);
  if (!dup) {
    s_lastPlayerId = f.playerId;
    s_lastShotSeq = f.shotSeq;
    s_lastShotAtMs = now;
    startDecodeFlash();
  }
  // 串口：每次成功解码都打印全部信息（含原始 5 字节与字段拆解）
  uint8_t b[5];
  for (int k = 0; k < 5; k++) b[k] = (raw >> (8 * (4 - k))) & 0xFF;
  Serial.printf(
      "[decode] ch%d t=%lums raw=%02X%02X%02X%02X%02X chkOK "
      "P=%u(%04X) W=%u T=%u S=%u\n",
      f.channel, (unsigned long)now, b[0], b[1], b[2], b[3], b[4],
      f.playerId, f.playerId, f.weaponId, f.team, f.shotSeq);
}

// 开机扫灯自检：逐颗点亮并停留，用于核对 LED 与位置的对应关系（0..14 = IRM1..15 同格）
static void ledSelfTest() {
#if SELFTEST_ENABLE
  if (SERIAL_DEBUG) Serial.println("[selftest] LED position check, one by one 0..14:");
  for (uint8_t i = 0; i < CELL_COUNT; i++) {
    fill_solid(s_leds, CELL_COUNT, CRGB::Black);
    s_leds[i] = SELFTEST_COLOR;
    FastLED.show();
    if (SERIAL_DEBUG) {
      Serial.printf("[selftest] LED%d ON: %s = IRM%u cell (GPIO%u)\n",
                    i, s_ledRef[i], i + 1, s_pins[i]);
    }
    delay(SELFTEST_STEP_MS);
  }
  fill_solid(s_leds, CELL_COUNT, LED_IDLE_COLOR);
  FastLED.show();
  if (SERIAL_DEBUG) Serial.println("[selftest] done");
#endif
}

void setup() {
  Serial.begin(115200);
  if (SERIAL_DEBUG) {
    delay(300);
    Serial.println();
    Serial.println("[cal-target] boot");
    Serial.printf("[cal-target] %u cells, LED_DATA=GPIO%d\n", CELL_COUNT, PIN_LED_DATA);
    Serial.printf("[cal-target] IRM GPIOs: ");
    for (uint8_t i = 0; i < CELL_COUNT; i++) {
      Serial.printf("IRM%u=GPIO%u ", i + 1, s_pins[i]);
    }
    Serial.println();
    Serial.printf("[cal-target] U8(IRM8 idx7)=GPIO%d 完整帧解码: 成功→蓝闪%u次\n",
                  IRM_GPIO_DECODE, DECODE_FLASH_TIMES);
  }

  for (uint8_t i = 0; i < CELL_COUNT; i++) {
    if (i == DECODE_CELL_IDX) continue;  // 解码格引脚由 FrameRx 接管
    pinMode(s_pins[i], INPUT_PULLUP);
    s_loStartUs[i] = 0;
    s_lastHitUs[i] = 0;
  }
  s_hit7AtMs = 0;

  // 灯串：WS2812C-2020（800kHz 单线），GPIO41 → 74HCT1G125 → +5V DIN
  FastLED.addLeds<LED_TYPE, PIN_LED_DATA>(s_leds, CELL_COUNT);
  FastLED.setBrightness(FASTLED_BRIGHTNESS);
  fill_solid(s_leds, CELL_COUNT, LED_IDLE_COLOR);
  FastLED.show();

  // 开机扫灯自检（逐颗核对位置，期间未挂接收中断，不受干扰）
  ledSelfTest();

  // U8 解码格：FrameRx 通道0（CHANGE 中断采集边沿 → 软件解码）
  frameRx.begin(IRM_GPIO_DECODE, -1);  // -1：单通道（无 56k 第二路）

  // 其余 14 格挂 CHANGE 中断（量测有效突发；格7=U8 由 FrameRx 接管）
  for (uint8_t i = 0; i < CELL_COUNT; i++) {
    if (i == DECODE_CELL_IDX) continue;
    switch (i) {
      case 0: IRM_ATTACH(0); break;
      case 1: IRM_ATTACH(1); break;
      case 2: IRM_ATTACH(2); break;
      case 3: IRM_ATTACH(3); break;
      case 4: IRM_ATTACH(4); break;
      case 5: IRM_ATTACH(5); break;
      case 6: IRM_ATTACH(6); break;
      case 8: IRM_ATTACH(8); break;
      case 9: IRM_ATTACH(9); break;
      case 10: IRM_ATTACH(10); break;
      case 11: IRM_ATTACH(11); break;
      case 12: IRM_ATTACH(12); break;
      case 13: IRM_ATTACH(13); break;
      case 14: IRM_ATTACH(14); break;
    }
  }

  if (SERIAL_DEBUG) Serial.println("[cal-target] ready (idle=green, 38k=red, U8 full decode=blue x3)");
}

void loop() {
  static uint32_t s_lastRefresh = 0;
  static uint16_t s_lastMask = 0;

  // U8 帧解码轮询（边沿缓冲非阻塞）
  LaserFrame f;
  uint64_t raw = 0;
  if (frameRx.poll(f, &raw)) {
    onFrameDecoded(f, raw);
  }

  if (millis() - s_lastRefresh >= LED_REFRESH_MS) {
    s_lastRefresh = millis();
    updateLeds();

    if (SERIAL_DEBUG) {
      uint16_t mask = 0;
      for (uint8_t i = 0; i < CELL_COUNT; i++) {
        bool hit = (i == DECODE_CELL_IDX)
                       ? (millis() - s_hit7AtMs) < LED_HIT_HOLD_MS
                       : cellHit(i);
        if (hit) mask |= (uint16_t)1 << i;
      }
      if (mask != s_lastMask) {
        s_lastMask = mask;
        Serial.printf("[hit] mask=0x%04X", mask);
        if (mask) {
          Serial.print("  cells:");
          for (uint8_t i = 0; i < CELL_COUNT; i++) {
            if (mask & ((uint16_t)1 << i)) Serial.printf(" %u", i + 1);
          }
        }
        Serial.println();
      }
    }
  }
}
