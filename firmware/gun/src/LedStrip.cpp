#include "LedStrip.h"
#include "config.h"

LedStrip led;

void LedStrip::begin(uint8_t pin, uint8_t count) {
  // FastLED 数据引脚必须是编译期常量：用 config.h 的 PIN_LED_DATA 宏。
  // NEOPIXEL 模板仅 2 个模板参数（数据引脚 + 默认 GRB），与 WS2812 兼容。
  (void)pin;  // 运行时引脚参数不再使用（引脚由 PIN_LED_DATA 宏决定）
  _count = min(count, (uint8_t)4);
  FastLED.addLeds<NEOPIXEL, PIN_LED_DATA>(_leds, _count);
  FastLED.setBrightness(80);
  for (uint8_t i = 0; i < _count; i++) _leds[i] = CRGB::Black;
  FastLED.show();
}

void LedStrip::setState(bool alive, bool inMatch, bool matchEnd) {
  if (_flashOn) return;
  CRGB color = CRGB::Cyan;
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
