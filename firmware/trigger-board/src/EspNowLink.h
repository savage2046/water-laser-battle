#pragma once
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>

// ESP-NOW 链路（检测板 ↔ 枪端主控，2.4G 点对点）。
// 上报：开火脉冲/心跳/异常；接收：断电/恢复指令。
// 帧格式：{ FRAME_MAGIC, type, d0, d1 }（4 字节）。
class EspNowLink {
 public:
  void begin(const uint8_t peerMac[6]);

  void send(uint8_t type, uint8_t d0 = 0, uint8_t d1 = 0);

  // 轮询取接收帧：返回 true 且填充 type/d0
  bool received(uint8_t &type, uint8_t &d0);

 private:
  uint8_t _peer[6];
  struct Rx { uint8_t type, d0; };
  Rx _rx[8];
  volatile uint8_t _rxHead = 0, _rxTail = 0;

  void pushRx(uint8_t type, uint8_t d0);

  static void onRxCb(const uint8_t *mac, const uint8_t *data, int len);
  static void onTxCb(const uint8_t *mac, esp_now_send_status_t status);
  static EspNowLink *_self;
};
