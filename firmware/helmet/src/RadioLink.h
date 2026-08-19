#pragma once
#include <Arduino.h>
#include <RadioLib.h>

// 470MHz LoRa 链路：SX1262 SPI 直驱（RadioLib）。一帧一包（≤64 字节）。
class RadioLink {
 public:
  void begin();
  void send(const char *frame);
  void sendFmt(const char *fmt, ...);
  bool poll(char *out, size_t maxLen);
  bool isReady() { return _ready; }

  // TDMA MAC 直接访问 SX1262（独占使用；ALOHA 模式下勿与 send/poll 混用）
  SX1262 *getRadio() { return _radio; }

 private:
  SX1262 *_radio = nullptr;
  bool _ready = false;
  char _buf[96];
};

extern RadioLink radio;
