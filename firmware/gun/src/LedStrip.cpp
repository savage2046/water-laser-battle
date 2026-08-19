#include "LedStrip.h"

LedStrip led;

void LedStrip::begin(uint8_t pin, uint8_t count) {
  _count = min(count, (uint8_t)4);
  FastLED.addLeds<WS2812B, uint8_t, GRB>(_leds, _count);
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
