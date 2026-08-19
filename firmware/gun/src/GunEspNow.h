#pragma once
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>

// ESP-NOW 链路（枪端主控 ↔ 检测板，见 docs/trigger-sensor-study.md）。
// 接收：开火脉冲（每发）→ 触发开火逻辑；心跳 → 在线监控。
// 发送：断电/恢复指令（阵亡/空弹 → 断电；重生/装弹完成 → 恢复）。
// 帧格式与检测板一致：{ FRAME_MAGIC, type, d0, d1 }。
class GunEspNow {
 public:
  void begin(const uint8_t boardMac[6]);

  // 取一个开火事件（每循环调用；返回 true 表示本循环应执行开火）
  bool takeFire() {
    if (_firePending == 0) return false;
    _firePending--;
    return true;
  }

  // 断电/恢复指令（ACK 发送，回调可查结果）
  void sendPower(bool on);

  bool linkAlive() { return _alive; }  // 检测板心跳在线
  void update();                       // 心跳超时检测（掉线置 false）

 private:
  static void onRxCb(const uint8_t *mac, const uint8_t *data, int len);
  static void onTxCb(const uint8_t *mac, esp_now_send_status_t status);
  void pushFire();

  uint8_t _peer[6];
  volatile uint8_t _firePending = 0;
  volatile uint32_t _lastHbMs = 0;
  volatile bool _alive = false;
  static GunEspNow *_self;
};
