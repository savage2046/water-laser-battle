#pragma once
#include <Arduino.h>

// 电流采样 + 扳机/连发识别状态机（见 docs/trigger-sensor-study.md §3）
//   空闲 →（电流>阈值 去抖 10ms）→ 发射中 →（每脉冲上升沿回调）→（低电流 50ms）→ 空闲
//   发射中持续高电流 > 2s → 堵转（回调，由 main 触发保护断电）
class CurrentSense {
 public:
  enum State { IDLE, FIRING, STALL };

  void begin();        // INA226 初始化或 ACS712 引脚
  float currentA();    // 读取当前电流（A）

  void update(uint32_t now);   // 每 SENSE_INTERVAL_MS 调用
  State state() { return _state; }

  // 回调（main.cpp 设置）
  void (*onFirePulse)(void) = nullptr;  // 每发（含第一发）
  void (*onFireEnd)(void) = nullptr;
  void (*onStall)(void) = nullptr;

  // 开机自校准：空闲基线（静置 500ms 取均值，扣除检测板自身耗电）
  void calibrateIdle(uint32_t ms);

 private:
  State _state = IDLE;
  bool _lastHigh = false;
  uint32_t _hiSince = 0, _loSince = 0, _lastPulse = 0;
  float _idleOffset = 0;
};
