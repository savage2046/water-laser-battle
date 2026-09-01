#pragma once
#include <Arduino.h>

// 枪回路断电开关：IO3 → U4.IN+（UCC27517 栅极驱动）→ OUT → Q1(20N03 N-MOS 低端)，
// HIGH=枪回路导通。IO3 为 LEDC PWM 引脚（可扩展为调速）。
// 默认导通（未收到指令时水弹枪正常使用）；断电后检测板自身独立取电仍在线。
class PowerSwitch {
 public:
  void begin() {
    pinMode(PIN_MOSFET, OUTPUT);
    digitalWrite(PIN_MOSFET, HIGH);  // 默认导通
    _on = true;
  }
  void on() {
    digitalWrite(PIN_MOSFET, HIGH);
    _on = true;
  }
  void off() {
    digitalWrite(PIN_MOSFET, LOW);   // 断开枪回路（水弹枪物理断电）
    _on = false;
  }
  bool isOn() { return _on; }

 private:
  bool _on = true;
};
