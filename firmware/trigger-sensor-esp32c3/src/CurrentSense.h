#pragma once
#include <Arduino.h>

// 电流采样 + 扳机/连发识别状态机（无改装水弹枪联动）
//   空闲 IDLE ──(电流>阈值 去抖 10ms)──► FIRING（第一发 onFirePulse）
//   FIRING ──(脉冲上升沿，距上发≥20ms)──► onFirePulse（连发逐发）
//   FIRING ──(持续高电流 ≥2s)──► STALL（堵转：onStall → main 断电）
//   FIRING ──(低电流持续 ≥50ms)──► IDLE（发射结束 onFireEnd）
//   STALL ──(电流回落或 5s 超时)──► IDLE
class CurrentSense {
 public:
  enum State { IDLE, FIRING, STALL };

  void begin();          // INA226 I2C 初始化（或 ACS712 ADC 引脚）
  float currentA();      // 读取当前瞬时电流（A，含空闲偏置）
  void update(uint32_t now);   // 主循环每 SENSE_INTERVAL_MS 调用
  State state() { return _state; }
  float lastCurrentA() const { return _lastA; }  // 最近一次采样的净电流（A，已扣空闲基线）

  // 回调（main.cpp 注入）
  void (*onFirePulse)(void) = nullptr;  // 每发（含第一发）
  void (*onFireEnd)(void) = nullptr;
  void (*onStall)(void) = nullptr;

  // 开机自校准：静置 ms 取均值 → 空闲基线（扣除检测板自身耗电）
  void calibrateIdle(uint32_t ms);

 private:
  State _state = IDLE;
  bool _lastHigh = false;
  uint32_t _hiSince = 0, _loSince = 0, _lastPulse = 0;
  float _idleOffset = 0;
  float _lastA = 0;   // 最近一次 update() 采样到的净电流（扣基线后）
};
