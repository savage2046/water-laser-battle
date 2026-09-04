#include <Arduino.h>
#include <U8g2lib.h>
#include <FastLED.h>
#include "config.h"
#include "FrameRx.h"

// ============================================================
// 校准靶 v3：5×3 二维阵列（XL-IRM ×15 + WS2812B ×15）
//   行主序 idx = row*ARRAY_COLS + col；中心格 ARR_CENTER_IDX 兼 38k 帧解码
// ============================================================

// 命中指示灯数组（FastLED，行主序）
static CRGB s_leds[LED_COUNT];

// 阵列载波检测标志（行主序 15 格）
static volatile bool g_hit[ARRAY_COUNT];
static const uint8_t g_pins[ARRAY_COUNT] = {
  ARR_RX0, ARR_RX1, ARR_RX2, ARR_RX3, ARR_RX4,
  ARR_RX5, ARR_RX6, ARR_RX7, ARR_RX8, ARR_RX9,
  ARR_RX10, ARR_RX11, ARR_RX12, ARR_RX13, ARR_RX14,
};

// 每格生成下降沿 ISR（宏展开）
#define ARR_ISR(n) \
  static void IRAM_ATTR arrISR##n() { g_hit[n] = true; }
ARR_ISR(0) ARR_ISR(1) ARR_ISR(2) ARR_ISR(3) ARR_ISR(4)
ARR_ISR(5) ARR_ISR(6) ARR_ISR(7) ARR_ISR(8) ARR_ISR(9)
ARR_ISR(10) ARR_ISR(11) ARR_ISR(12) ARR_ISR(13) ARR_ISR(14)

// ===== OLED =====
static U8G2_SSD1306_128X64_F_SW_I2C *u8g2;
static unsigned long g_lastDraw = 0;

// ===== 命中/解码状态 =====
static bool g_hitDecoded = false;   // 中心格解出完整帧
static LaserFrame g_frame;
static unsigned long g_hitAt = 0;
static int g_framesSeen = 0;
static uint16_t g_lastShotSeq = 0xFFFF;

// 命中灯刷新：命中格亮绿（中心格解码命中白闪）
static void updateLeds() {
  for (uint8_t i = 0; i < ARRAY_COUNT; i++) {
    s_leds[i] = g_hit[i] ? CRGB(LED_HIT_COLOR) : CRGB(0, LED_IDLE_DIM, 0);
  }
  if (g_hitDecoded && millis() - g_hitAt < LED_CENTER_FLASH_MS) {
    s_leds[ARR_CENTER_IDX] = CRGB::White;
  }
  FastLED.show();
}

static void beep(int ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
}

// 2D 统计：返回 (列数, 列质心%, 行数, 行质心%)，质心 0=中心，±100%=±一格(边缘)
static void stat2D(int &w, int &h, int &cx, int &cy) {
  bool colHit[ARRAY_COLS] = {false}, rowHit[ARRAY_ROWS] = {false};
  int total = 0;
  long sumC = 0, sumR = 0;
  for (uint8_t r = 0; r < ARRAY_ROWS; r++) {
    for (uint8_t c = 0; c < ARRAY_COLS; c++) {
      if (g_hit[r * ARRAY_COLS + c]) {
        colHit[c] = true;
        rowHit[r] = true;
        total++;
        sumC += c;
        sumR += r;
      }
    }
  }
  w = 0; h = 0;
  for (uint8_t c = 0; c < ARRAY_COLS; c++) if (colHit[c]) w++;
  for (uint8_t r = 0; r < ARRAY_ROWS; r++) if (rowHit[r]) h++;
  if (total == 0) { cx = 0; cy = 0; return; }
  int cMid = (ARRAY_COLS - 1) / 2;   // 2
  int rMid = (ARRAY_ROWS - 1) / 2;   // 1
  // 质心相对中心，单位格，转 %（±100 = ±1 格）
  cx = (int)((sumC * 100 / total) - cMid * 100) / 1;
  cy = (int)((sumR * 100 / total) - rMid * 100) / 1;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // 阵列引脚：下降沿中断置标志（中心格 ARR_CENTER_IDX 由 FrameRx CHANGE 解码，不挂 FALLING）
  for (uint8_t i = 0; i < ARRAY_COUNT; i++) {
    pinMode(g_pins[i], INPUT_PULLUP);
    g_hit[i] = false;
  }
  // 逐格挂 FALLING（跳过中心格）
  for (uint8_t i = 0; i < ARRAY_COUNT; i++) {
    if (i == ARR_CENTER_IDX) continue;
    switch (i) {
      case 0: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR0, FALLING); break;
      case 1: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR1, FALLING); break;
      case 2: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR2, FALLING); break;
      case 3: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR3, FALLING); break;
      case 4: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR4, FALLING); break;
      case 5: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR5, FALLING); break;
      case 6: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR6, FALLING); break;
      case 8: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR8, FALLING); break;
      case 9: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR9, FALLING); break;
      case 10: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR10, FALLING); break;
      case 11: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR11, FALLING); break;
      case 12: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR12, FALLING); break;
      case 13: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR13, FALLING); break;
      case 14: attachInterrupt(digitalPinToInterrupt(g_pins[i]), arrISR14, FALLING); break;
    }
  }

  frameRx.begin(PIN_IR_RX_940, PIN_IR_RX_850);   // 中心格解码 + 56k 预留

  u8g2 = new U8G2_SSD1306_128X64_F_SW_I2C(U8G2_R0, PIN_OLED_SCL,
                                           PIN_OLED_SDA, U8X8_PIN_NONE);
  u8g2->begin();
  u8g2->clear();
  u8g2->setFont(u8g2_font_6x10_tf);
  u8g2->drawStr(10, 26, "TARGET v3");
  u8g2->drawStr(10, 40, "5x3 grid");
  u8g2->sendBuffer();

  FastLED.addLeds<NEOPIXEL, PIN_LED_DATA>(s_leds, LED_COUNT);
  FastLED.setBrightness(80);
  FastLED.show();

  Serial.println("[target] boot v3");
}

void loop() {
  // 中心格"有信号"活动标志（FrameRx 通道0 任意边沿）→ 与其它格一致的"收到"显示
  if (frameRx.act[0]) {
    g_hit[ARR_CENTER_IDX] = true;
    frameRx.act[0] = false;
  }

  // 中心格帧解码：成功 → 命中（去重按 shotSeq 判双帧）
  LaserFrame f;
  if (frameRx.poll(f)) {
    g_hit[ARR_CENTER_IDX] = true;
    bool sameShot = (f.playerId == g_frame.playerId) &&
                    (f.shotSeq == g_lastShotSeq);
    if (!sameShot) {
      g_frame = f;
      g_lastShotSeq = f.shotSeq;
      g_framesSeen = 1;
      g_hitAt = millis();
      g_hitDecoded = true;
      beep(60);
      Serial.printf("[hit] P%u W%u T%u S%u\n", f.playerId, f.weaponId, f.team,
                    f.shotSeq);
    } else {
      g_framesSeen++;
    }
  }

  // 命中窗口过期：清空全部格标志
  if (g_hitDecoded && millis() - g_hitAt > HIT_WINDOW_MS) {
    g_hitDecoded = false;
    for (uint8_t i = 0; i < ARRAY_COUNT; i++) g_hit[i] = false;
    g_framesSeen = 0;
  }

  // OLED + LED 节流刷新
  if (millis() - g_lastDraw >= DISPLAY_REFRESH_MS) {
    g_lastDraw = millis();
    updateLeds();
    u8g2->clearBuffer();
    u8g2->setFont(u8g2_font_6x10_tf);

    char line[24];
    if (!g_hitDecoded) {
      u8g2->drawStr(2, 24, "TARGET READY");
      u8g2->drawStr(2, 36, "aim 940 beam");
      u8g2->drawStr(2, 48, "center of grid");
    } else {
      // 6 行 × 10px 高：y 起点 2..52（顶部对齐，不溢出 64px）
      // 行1 解码
      snprintf(line, sizeof(line), "P%u W%u T%u S%u", g_frame.playerId,
               g_frame.weaponId, g_frame.team, g_frame.shotSeq);
      u8g2->drawStr(2, 2, line);

      // 行2 强度 + span
      int w, h, cx, cy;
      stat2D(w, h, cx, cy);
      snprintf(line, sizeof(line), "%s span%dx%d", g_framesSeen >= STRONG_FRAMES ? "STRONG" : "WEAK", w, h);
      u8g2->drawStr(2, 12, line);

      // 行3-5 阵列图（三行各 5 格，row0 在顶）
      for (uint8_t r = 0; r < ARRAY_ROWS; r++) {
        char row[6];
        for (uint8_t c = 0; c < ARRAY_COLS; c++) {
          row[c] = g_hit[r * ARRAY_COLS + c] ? 'X' : '.';
        }
        row[ARRAY_COLS] = '\0';
        u8g2->drawStr(2, 22 + r * 10, row);
      }

      // 行6 质心偏移（0=对中；±100 ≈ ±1 格）
      snprintf(line, sizeof(line), "x%+d%% y%+d%%", cx, cy);
      u8g2->drawStr(2, 52, line);
    }
    u8g2->sendBuffer();
  }

  delay(1);
}
