#pragma once
#include <Arduino.h>
#include <U8g2lib.h>

// 网关显示屏：SSD1306 OLED 128x64 I2C（SW_I2C 支持任意 SDA/SCL 引脚）
// 显示：网关 ID、WS 连接状态、已学习设备数、最近命中事件（组播/本地）
class GwDisplay {
 public:
  void begin(uint8_t sda, uint8_t scl, uint8_t addr);

  // 定期调用：基础信息（连接状态、设备数）
  void update(bool wsConnected, uint8_t devCount, const char *ip);

  // 事件到达时立即调用（本地 915M 或组播），显示最近命中
  void showEvent(const char *kind, uint8_t devIdx, uint16_t seq,
                 uint16_t p1, uint16_t p2);

 private:
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C *_u8g2 = nullptr;
  unsigned long _lastDraw = 0;
  bool _lastWs = false;
  char _evLine1[22] = "---";
  char _evLine2[22] = "";
};

extern GwDisplay gwDisplay;
