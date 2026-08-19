#pragma once
#include <Arduino.h>
#include <RadioLib.h>

// 470MHz LoRa 链路：SX1262 SPI 直驱（RadioLib）。
// 一帧一个 LoRa 包（≤64 字节），LoRa 自带长度，无需分隔符。
class RadioLink {
 public:
  // 初始化 SX1262（参数来自 config.h：频率/带宽/SF/CR/同步字/功率）
  void begin();

  // 发送一帧（阻塞至发送完成，随后回到连续接收）
  void send(const char *frame);
  void sendFmt(const char *fmt, ...);

  // 非阻塞轮询接收：返回 true 表示收到一帧
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
