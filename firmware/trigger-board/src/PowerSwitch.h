#pragma once
#include <Arduino.h>

// 枪回路断电开关（MOSFET 串联电池回路，HIGH=导通）。
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
