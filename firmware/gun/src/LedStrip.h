#pragma once
#include <Arduino.h>

// 状态灯：**模组板载 G48 单色 LED**（灌电流接法，低电平点亮）。
//
// ⚠️ 2026-09-13 由"G8 上的外置 WS2812（FastLED 多色）"改为本实现。原因：
//   · 映射表里 `LED_DATA（WS2812）= G08` 标的是 🆕 —— **规划但从未接线**
//     （`PCB/gun-board/main-board-S3引脚映射.md` §1 与 §4 待办第 1 条"LED=G08 待接线"）
//     → 那颗灯在硬件上不存在，FastLED 驱动它是白费；
//   · 板上实际能亮的只有**模组自带**的 G48，而且是**普通单色 LED，不是 WS2812**
//     （灌电流：阳极→3.3V、阴极→限流电阻→GPIO48 → **低电平点亮**，载板无需接线；
//      见 `PCB/gun-board/main-ESP32主控-U1管脚定义-在线读取.md` 与
//      `docs/gun-selftest-联调记录.md`）。单色灯不认 WS2812 协议，故不能用 FastLED。
//
// 单色灯没有颜色语义 → 原来多色表达的"状态"改为**闪烁节奏**：
//   待机（未开局）   慢闪 1Hz
//   开局·存活        常亮
//   开局·阵亡        快闪 4Hz
//   对局结束         慢闪 0.5Hz
//   命中闪光         常亮 200ms（压过上面，给即时反馈）
//
// 接口（begin / setState / flashHit / update）与原 WS2812 版**保持一致**，
// 调用点（gun/src/main.cpp）无需改动，只是不再有颜色参数的含义。
//
// 若将来真的焊上外置 WS2812 灯带：重新加 `fastled/FastLED` 依赖，
// 恢复本文件的 FastLED 版实现，并把 PIN_LED 改回灯带的数据脚。
class LedStrip {
 public:
  void begin(uint8_t pin, uint8_t count);
  void setState(bool alive, bool inMatch, bool matchEnd);
  void flashHit();
  void update();

 private:
  bool _alive = false;
  bool _inMatch = false;
  bool _matchEnd = false;
  bool _flashOn = false;
  unsigned long _flashUntil = 0;
};

extern LedStrip led;
