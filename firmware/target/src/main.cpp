#include <Arduino.h>
#include <U8g2lib.h>
#include "config.h"
#include "FrameRx.h"

// ===== 阵列载波检测：9 颗 TSOP，下降沿置标志 =====
static volatile bool g_arrHit[ARRAY_COUNT];
static const uint8_t g_arrPins[ARRAY_COUNT] = {
  ARRAY_RX0, ARRAY_RX1, ARRAY_RX2, ARRAY_RX3, ARRAY_RX4,
  ARRAY_RX5, ARRAY_RX6, ARRAY_RX7, ARRAY_RX8,
};

// 为每颗生成下降沿 ISR（宏展开）
#define ARR_ISR(n) \
  static void IRAM_ATTR arrISR##n() { g_arrHit[n] = true; }
ARR_ISR(0) ARR_ISR(1) ARR_ISR(2) ARR_ISR(3) ARR_ISR(4)
ARR_ISR(5) ARR_ISR(6) ARR_ISR(7) ARR_ISR(8)

// ===== OLED =====
static U8G2_SSD1306_128X64_F_SW_I2C *u8g2;
static unsigned long g_lastDraw = 0;

// ===== 命中状态 =====
static bool g_hit = false;
static LaserFrame g_frame;
static unsigned long g_hitAt = 0;
static int g_framesSeen = 0;   // 双帧中收到几帧（强/弱判断）
static unsigned long g_lastFrameMs = 0;
static uint16_t g_lastShotSeq = 0xFFFF;

static void beep(int ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // 阵列引脚：下降沿中断置标志
  // 注意：中心颗 ARRAY_RX4(GPIO34) 不挂 FALLING——它由 FrameRx 的 CHANGE
  // 中断占用做帧解码；其"收到载波"标志在解码成功时置位
  for (uint8_t i = 0; i < ARRAY_COUNT; i++) {
    pinMode(g_arrPins[i], INPUT);
    g_arrHit[i] = false;
  }
  attachInterrupt(digitalPinToInterrupt(ARRAY_RX0), arrISR0, FALLING);
  attachInterrupt(digitalPinToInterrupt(ARRAY_RX1), arrISR1, FALLING);
  attachInterrupt(digitalPinToInterrupt(ARRAY_RX2), arrISR2, FALLING);
  attachInterrupt(digitalPinToInterrupt(ARRAY_RX3), arrISR3, FALLING);
  attachInterrupt(digitalPinToInterrupt(ARRAY_RX5), arrISR5, FALLING);
  attachInterrupt(digitalPinToInterrupt(ARRAY_RX6), arrISR6, FALLING);
  attachInterrupt(digitalPinToInterrupt(ARRAY_RX7), arrISR7, FALLING);
  attachInterrupt(digitalPinToInterrupt(ARRAY_RX8), arrISR8, FALLING);

  frameRx.begin(PIN_IR_RX_940, PIN_IR_RX_850);

  u8g2 = new U8G2_SSD1306_128X64_F_SW_I2C(U8G2_R0, PIN_OLED_SCL,
                                           PIN_OLED_SDA, U8X8_PIN_NONE);
  u8g2->begin();
  u8g2->clear();
  u8g2->setFont(u8g2_font_6x10_tf);
  u8g2->drawStr(15, 30, "TARGET READY");
  u8g2->drawStr(15, 44, "aim & fire");
  u8g2->sendBuffer();

  Serial.println("[target] boot");
}

void loop() {
  // 帧解码：拼合成功 → 命中（中心颗收到载波标志同时置位）
  LaserFrame f;
  if (frameRx.poll(f)) {
    g_arrHit[4] = true;  // 中心颗（ARRAY_RX4）收到
    bool sameShot = (f.playerId == g_frame.playerId) &&
                    (f.shotSeq == g_lastShotSeq);
    if (!sameShot) {
      // 新的一发：记录命中
      g_frame = f;
      g_lastShotSeq = f.shotSeq;
      g_framesSeen = 1;
      g_hitAt = millis();
      g_hit = true;
      beep(60);
      Serial.printf("[hit] P%u W%u T%u S%u\n", f.playerId, f.weaponId, f.team,
                    f.shotSeq);
    } else {
      g_framesSeen++;  // 双帧第二帧（同 shotSeq）
    }
    g_lastFrameMs = millis();
  }

  // 命中窗口过期：清空
  if (g_hit && millis() - g_hitAt > HIT_WINDOW_MS) {
    g_hit = false;
    for (uint8_t i = 0; i < ARRAY_COUNT; i++) g_arrHit[i] = false;
    g_framesSeen = 0;
  }

  // OLED 节流刷新
  if (millis() - g_lastDraw >= DISPLAY_REFRESH_MS) {
    g_lastDraw = millis();
    u8g2->clearBuffer();
    u8g2->setFont(u8g2_font_6x10_tf);

    if (!g_hit) {
      u8g2->drawStr(2, 12, "TARGET READY");
      u8g2->drawStr(2, 26, "aim & fire");
    } else {
      // 行1：解码结果
      char line1[24];
      snprintf(line1, sizeof(line1), "P%u W%u T%u S%u", g_frame.playerId,
               g_frame.weaponId, g_frame.team, g_frame.shotSeq);
      u8g2->drawStr(2, 12, line1);

      // 行2：强度
      char line2[16];
      snprintf(line2, sizeof(line2), "%s", g_framesSeen >= STRONG_FRAMES ? "STRONG" : "WEAK");
      u8g2->drawStr(2, 24, line2);

      // 行3-4：靶面阵列图（9 颗）
      char row[16];
      uint8_t span = 0;
      for (uint8_t i = 0; i < ARRAY_COUNT; i++) {
        if (g_arrHit[i]) {
          row[i] = 'X';
          span++;
        } else {
          row[i] = '.';
        }
      }
      row[ARRAY_COUNT] = '\0';
      u8g2->drawStr(2, 40, row);

      // 行5：宽度与位置
      char line5[24];
      uint8_t centerOfMass = 0, cnt = 0;
      for (uint8_t i = 0; i < ARRAY_COUNT; i++) {
        if (g_arrHit[i]) {
          centerOfMass += i;
          cnt++;
        }
      }
      int pos = cnt ? (int)(centerOfMass * 100 / cnt) - 400 : -1;  // -400..400 相对中心%
      snprintf(line5, sizeof(line5), "span %u  pos %+d%%", span, pos);
      u8g2->drawStr(2, 56, line5);
    }
    u8g2->sendBuffer();
  }

  delay(1);
}
