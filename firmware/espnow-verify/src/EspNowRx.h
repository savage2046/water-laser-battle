#pragma once
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_mac.h>

// ============================================================
// ESP-NOW 接收/发送封装（纯传输层，统计/打印在 main.cpp）
// 帧格式与检测板一致：{ 0xA5, type, d0, d1 }（4 字节）
// 兼容 Arduino core 2.x / 3.x：新内核回调带 RSSI，旧内核为 0（不可用）
// ============================================================

struct RxFrame {
  uint8_t mac[6];    // 源 MAC（发送端）
  uint8_t data[4];   // 原始帧（含 magic）
  int len;           // 实际长度（≤4）
  int8_t rssi;       // 接收信号强度 dBm（旧内核不支持时为 0）
};

class EspNowRx {
 public:
  bool begin();                // MAC 伪装 + WiFi + ESP-NOW；成功返回 true
  bool received(RxFrame &f);   // 轮询取一帧（ring 队列，满则丢新帧）
  void sendPower(bool on);     // 下行：FRAME_POWER 0=断 1=通（广播或单播）
  uint32_t txOk() const { return _txOk; }
  uint32_t txFail() const { return _txFail; }
  bool macSpoofed() const { return _macSpoofed; }

 private:
  static const uint8_t kRxRing = 32;
  RxFrame _ring[kRxRing];
  volatile uint8_t _head = 0, _tail = 0;
  uint8_t _peer[6];
  bool _peerUnicast = false;
  volatile uint32_t _txOk = 0, _txFail = 0;
  bool _macSpoofed = false;

  void pushRx(const uint8_t *mac, const uint8_t *data, int len, int8_t rssi);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  static void onRxCb(const esp_now_recv_info_t *info, const uint8_t *data, int len);
#else
  static void onRxCb(const uint8_t *mac, const uint8_t *data, int len);
#endif
  static void onTxCb(const uint8_t *mac, esp_now_send_status_t status);
  static EspNowRx *_self;
};
