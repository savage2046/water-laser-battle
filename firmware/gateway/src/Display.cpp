#include "Display.h"
#include "config.h"

GwDisplay gwDisplay;

void GwDisplay::begin(uint8_t sda, uint8_t scl, uint8_t addr) {
  _u8g2 = new U8G2_SSD1306_128X64_NONAME_F_SW_I2C(U8G2_R0, scl, sda,
                                                   U8X8_PIN_NONE);
  _u8g2->setI2CAddress(addr << 1);  // U8g2 需要 8 位地址（0x3C → 0x78）
  _u8g2->begin();
  _u8g2->clear();
  _u8g2->setFont(u8g2_font_6x10_tf);
  _u8g2->drawStr(20, 30, "GW BOOT...");
  _u8g2->sendBuffer();
}

void GwDisplay::update(bool wsConnected, uint8_t devCount, const char *ip) {
  // 节流：500ms 一帧；连接状态变化立即刷新
  if (!(wsConnected != _lastWs) && millis() - _lastDraw < 500) return;
  _lastDraw = millis();
  _lastWs = wsConnected;

  _u8g2->clearBuffer();
  _u8g2->setFont(u8g2_font_6x10_tf);

  // 行1：网关 ID + WS 状态
  char line1[32];
  snprintf(line1, sizeof(line1), "%s %s", GATEWAY_ID,
           wsConnected ? "ONLINE" : "OFFLINE");
  _u8g2->drawStr(2, 12, line1);

  // 行2：IP + 设备数
  char line2[32];
  snprintf(line2, sizeof(line2), "%s DEV:%u", ip ? ip : "...", devCount);
  _u8g2->drawStr(2, 26, line2);

  // 行3-4：最近命中事件
  _u8g2->drawStr(2, 44, "EVENT:");
  _u8g2->drawStr(2, 58, _evLine1);
  if (_evLine2[0]) _u8g2->drawStr(2, 62, _evLine2);

  _u8g2->sendBuffer();
}

void GwDisplay::showEvent(const char *kind, uint8_t devIdx, uint16_t seq,
                          uint16_t p1, uint16_t p2) {
  char line1[22];
  if (strcmp(kind, "H") == 0) {
    snprintf(line1, sizeof(line1), "HIT D%u <- P%u", devIdx, p1);
    snprintf(_evLine2, sizeof(_evLine2), "HP%u seq%u", p2, seq);
  } else if (strcmp(kind, "D") == 0) {
    snprintf(line1, sizeof(line1), "DEAD D%u by P%u", devIdx, p1);
    snprintf(_evLine2, sizeof(_evLine2), "seq%u", seq);
  } else if (strcmp(kind, "C") == 0) {
    snprintf(line1, sizeof(line1), "CAPTURE D%u", devIdx);
    snprintf(_evLine2, sizeof(_evLine2), "seq%u", seq);
  } else if (strcmp(kind, "T") == 0) {
    snprintf(line1, sizeof(line1), "TAG D%u by P%u", devIdx, p1);
    snprintf(_evLine2, sizeof(_evLine2), "seq%u", seq);
  } else {
    snprintf(line1, sizeof(line1), "%s D%u seq%u", kind, devIdx, seq);
    _evLine2[0] = '\0';
  }
  snprintf(_evLine1, sizeof(_evLine1), "%s", line1);
  // 置零节流时间戳：主循环下一轮 update() 立即重绘完整画面
  _lastDraw = 0;
}
