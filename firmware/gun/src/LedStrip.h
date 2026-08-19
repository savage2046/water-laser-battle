#pragma once
#include <Arduino.h>
#include <FastLED.h>

// 多色状态灯（WS2812）：存活绿 / 阵亡红 / 命中闪白 / 开局蓝 / 结束黄 / 待机青
class LedStrip {
 public:
  void begin(uint8_t pin, uint8_t count);
  void setState(bool alive, bool inMatch, bool matchEnd);
  void flashHit();
  void update();

 private:
  CRGB _leds[4];
  uint8_t _count = 0;
  unsigned long _flashUntil = 0;
  bool _flashOn = false;
};

extern LedStrip led;
