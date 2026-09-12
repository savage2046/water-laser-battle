#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================
// 枪回路断电开关（防作弊硬保证）
//   IO3(HIGH) → UCC27517.IN+ → OUT → Q1 = 20N03（N-MOS 低端，TO-252）
//   HIGH = 枪回路导通（水弹枪正常供电）
//   LOW  = 断开（水弹枪物理断电，无法发射）
// 检测板自身经 XL1509/AMS1117 独立供电，断电后仍在线可收恢复指令。
// 默认导通：未收到指令时水弹枪正常使用。
// ============================================================
class PowerSwitch {
 public:
  void begin() {
    pinMode(PIN_MOSFET, OUTPUT);
    on();                        // 默认导通
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
  bool _on = false;
};
