#include "GunEspNow.h"
#include "config.h"
#include <WiFi.h>

GunEspNow *GunEspNow::_self = nullptr;

void GunEspNow::begin(const uint8_t boardMac[6]) {
  memcpy(_peer, boardMac, 6);
  _firePending = 0;
  _alive = false;
  _self = this;

  WiFi.mode(WIFI_STA);                      // 仅初始化 2.4G 栈（不连接；与 470M 无关）
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_now_init();
  esp_now_register_recv_cb(onRxCb);
  esp_now_register_send_cb(onTxCb);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, _peer, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void GunEspNow::sendPower(bool on) {
  uint8_t buf[4] = { 0xA5, 0x10, on ? 0x01 : 0x00, 0 };
  esp_now_send(_peer, buf, sizeof(buf));
}

void GunEspNow::update() {
  // 检测板心跳 1Hz；3s 无心跳 → 判定掉线（枪可用性不受影响，仅告警/上报）
  if (_alive && millis() - _lastHbMs > 3000) {
    _alive = false;
    Serial.println("[espnow] trigger board link lost");
  }
}

void GunEspNow::pushFire() {
  if (_firePending < 32) _firePending++;   // 上限防溢出（连发 20Hz ≪ 主循环消费速率）
}

void GunEspNow::onRxCb(const uint8_t *mac, const uint8_t *data, int len) {
  if (!_self || len < 4) return;
  if (data[0] != 0xA5) return;
  switch (data[1]) {
    case 0x01:  // FRAME_FIRE：开火脉冲 → 触发开火逻辑
      _self->pushFire();
      break;
    case 0x02:  // FRAME_HB：心跳
      _self->_lastHbMs = millis();
      _self->_alive = true;
      break;
    case 0x03:  // FRAME_FAULT：检测板堵转保护断电（打印告警）
      Serial.printf("[espnow] board fault code=%u\n", data[2]);
      break;
    default:
      break;
  }
}

void GunEspNow::onTxCb(const uint8_t *mac, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.printf("[espnow] tx fail\n");
  }
}
