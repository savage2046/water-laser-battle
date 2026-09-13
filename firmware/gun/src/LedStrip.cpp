#include "LedStrip.h"
#include "config.h"

LedStrip led;

// G48 是**灌电流**接法（阳极→3.3V、阴极→限流电阻→GPIO48）→ **低电平点亮**，
// 所以写 HIGH 是"灭"、写 LOW 才是"亮"。LED_ON_LEVEL 在 config.h 里定义为 LOW。
static inline void ledWrite(bool on) {
  digitalWrite(PIN_LED, on ? LED_ON_LEVEL : (LED_ON_LEVEL == LOW ? HIGH : LOW));
}

void LedStrip::begin(uint8_t pin, uint8_t count) {
  // 单色灯：数据引脚由编译期 PIN_LED 决定；形参保留是为了不改调用点。
  if (pin != PIN_LED) {
    Serial.printf("[led] 提示：状态灯固定用 PIN_LED=G%d，忽略传入的 G%u\n",
                  (int)PIN_LED, (unsigned)pin);
  }
  (void)count;
  pinMode(PIN_LED, OUTPUT);
  ledWrite(false);   // 开机先灭
}

void LedStrip::setState(bool alive, bool inMatch, bool matchEnd) {
  _alive = alive;
  _inMatch = inMatch;
  _matchEnd = matchEnd;
  if (_flashOn) return;   // 命中闪光期间不下手，交给 update()
  update();               // 立即生效（与原 setState 直接上色一致）
}

void LedStrip::flashHit() {
  _flashOn = true;
  _flashUntil = millis() + 200;
  ledWrite(true);
}

// 单色灯的唯一自由度就是"亮/灭 × 节奏"，状态 → 节奏的映射见 LedStrip.h 顶部注释。
void LedStrip::update() {
  const unsigned long now = millis();

  if (_flashOn) {
    if (now < _flashUntil) {   // 命中：常亮 200ms
      ledWrite(true);
      return;
    }
    _flashOn = false;
  }

  if (_matchEnd) {             // 对局结束：慢闪 0.5Hz
    ledWrite(((now / 1000) & 1) == 0);
    return;
  }
  if (!_inMatch) {             // 待机（未开局）：慢闪 1Hz
    ledWrite(((now / 500) & 1) == 0);
    return;
  }
  if (_alive) {                // 开局·存活：常亮
    ledWrite(true);
    return;
  }
  ledWrite(((now / 125) & 1) == 0);   // 开局·阵亡：快闪 4Hz
}
