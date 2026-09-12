#include "LedStrip.h"
#include "config.h"

LedStrip led;

void LedStrip::begin(uint8_t pin, uint8_t count) {
  // ⚠️ FastLED 的数据脚必须是**编译期常量**（模板参数），运行时变量用不了。
  //    所以这里固定用 config.h 的 PIN_LED；传入的 pin 只做一致性校验。
  //    （历史写法 `addLeds<WS2812B, uint8_t, GRB>(...)` 把"类型"当成了引脚模板参数，
  //      FastLED 3.10 直接报 wrong number of template arguments / no matching function。）
  if (pin != PIN_LED)
    Serial.printf("[led] 警告：FastLED 数据脚固定用编译期 PIN_LED=G%d，忽略传入的 G%u\n",
                  (int)PIN_LED, (unsigned)pin);
  _count = min(count, (uint8_t)4);
  FastLED.addLeds<WS2812B, PIN_LED, GRB>(_leds, _count);
  FastLED.setBrightness(80);
  for (uint8_t i = 0; i < _count; i++) _leds[i] = CRGB::Black;
  FastLED.show();
}

void LedStrip::setState(bool alive, bool inMatch, bool matchEnd) {
  if (_flashOn) return;  // 闪烁优先
  CRGB color = CRGB::Cyan;  // 待机
  if (matchEnd) color = CRGB::Yellow;
  else if (inMatch) color = alive ? CRGB::Green : CRGB::Red;
  for (uint8_t i = 0; i < _count; i++) _leds[i] = color;
  FastLED.show();
}

void LedStrip::flashHit() {
  _flashOn = true;
  _flashUntil = millis() + 200;
  for (uint8_t i = 0; i < _count; i++) _leds[i] = CRGB::White;
  FastLED.show();
}

void LedStrip::update() {
  if (_flashOn && millis() >= _flashUntil) {
    _flashOn = false;
  }
}
