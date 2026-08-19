#include "EspNowLink.h"
#include "config.h"
#include <WiFi.h>

EspNowLink *EspNowLink::_self = nullptr;

void EspNowLink::begin(const uint8_t peerMac[6]) {
  memcpy(_peer, peerMac, 6);
  _rxHead = _rxTail = 0;
  _self = this;

  WiFi.mode(WIFI_STA);                      // 仅初始化 2.4G 栈，不连接
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

void EspNowLink::send(uint8_t type, uint8_t d0, uint8_t d1) {
  uint8_t buf[4] = { FRAME_MAGIC, type, d0, d1 };
  esp_now_send(_peer, buf, sizeof(buf));
}

bool EspNowLink::received(uint8_t &type, uint8_t &d0) {
  if (_rxHead == _rxTail) return false;
  Rx r = _rx[_rxHead];
  _rxHead = (_rxHead + 1) & 7;
  type = r.type;
  d0 = r.d0;
  return true;
}

void EspNowLink::pushRx(uint8_t type, uint8_t d0) {
  uint8_t next = (_rxTail + 1) & 7;
  if (next == _rxHead) return;  // 满：丢弃
  _rx[_rxTail].type = type;
  _rx[_rxTail].d0 = d0;
  _rxTail = next;
}

void EspNowLink::onRxCb(const uint8_t *mac, const uint8_t *data, int len) {
  if (!_self || len < 4) return;
  if (data[0] != FRAME_MAGIC) return;
  _self->pushRx(data[1], data[2]);
}

void EspNowLink::onTxCb(const uint8_t *mac, esp_now_send_status_t status) {
  // ACK 结果（打印可选）
}
