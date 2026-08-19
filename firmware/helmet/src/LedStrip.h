#pragma once
#include <Arduino.h>
#include <FastLED.h>

// 多色状态灯（WS2812）：存活绿 / 阵亡红 / 命中闪白 / 开局蓝 / 结束黄 / 待机青
class LedStrip {
 public:
  void begin(uint8_t pin, uint8_t count);

  // 状态灯：standby/matchEnd 常亮对应色；alive 绿、dead 红
  void setState(bool alive, bool inMatch, bool matchEnd);

  // 命中闪烁（白闪 200ms）
  void flashHit();

  // 每循环调用（处理闪烁时序）
  void update();

 private:
  CRGB _leds[4];
  uint8_t _count = 0;
  unsigned long _flashUntil = 0;
  bool _flashOn = false;
};

extern LedStrip led;
