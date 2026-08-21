#include "EspNowRx.h"
#include "config.h"
#include <WiFi.h>

EspNowRx *EspNowRx::_self = nullptr;

bool EspNowRx::begin() {
  _self = this;
  _head = _tail = 0;
  _macSpoofed = false;

  // ---- 1) MAC 伪装：把自己伪装成枪端 MAC，直接接收检测板单播帧 ----
#if SPOOF_GUN_MAC
  static const uint8_t gunMac[6] = GUN_MAC;
  uint8_t factoryMac[6] = {0};
  esp_efuse_mac_get_default(factoryMac);
  if (memcmp(factoryMac, gunMac, 6) == 0) {
    _macSpoofed = true;                        // 本机就是枪端，无需改动
  } else if (esp_base_mac_addr_set(gunMac) == ESP_OK) {
    _macSpoofed = true;                        // 开机前改基址 MAC（重启还原）
  }
#endif

  WiFi.mode(WIFI_STA);                         // 仅初始化 2.4G 栈，不连接
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

#if SPOOF_GUN_MAC
  if (!_macSpoofed) {                          // 兜底：接口启动后再设 STA MAC
    static const uint8_t gunMac2[6] = GUN_MAC;
    _macSpoofed = (esp_wifi_set_mac(WIFI_IF_STA, (uint8_t *)gunMac2) == ESP_OK);
  }
#endif

  const esp_err_t e = esp_now_init();
  if (e != ESP_OK) return false;
  esp_now_register_recv_cb(onRxCb);
  esp_now_register_send_cb(onTxCb);

  // ---- 2) 下行目标：默认广播；填了真实 MAC 则加 peer 后单播 ----
  static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  static const uint8_t boardMac[6] = BOARD_MAC;
  _peerUnicast = (memcmp(boardMac, bcast, 6) != 0);
  if (_peerUnicast) {
    memcpy(_peer, boardMac, 6);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, _peer, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  return true;
}

bool EspNowRx::received(RxFrame &f) {
  if (_head == _tail) return false;
  f = _ring[_head];
  _head = (_head + 1) % kRxRing;
  return true;
}

void EspNowRx::sendPower(bool on) {
  uint8_t buf[4] = { FRAME_MAGIC, FRAME_POWER, (uint8_t)(on ? 0x01 : 0x00), 0x00 };
  static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  const uint8_t *dst = _peerUnicast ? _peer : bcast;
  esp_now_send(dst, buf, sizeof(buf));
}

void EspNowRx::pushRx(const uint8_t *mac, const uint8_t *data, int len, int8_t rssi) {
  if (!mac) return;
  const uint8_t next = (_tail + 1) % kRxRing;
  if (next == _head) return;                   // 满：丢弃（与 EspNowLink 一致）
  memcpy(_ring[_tail].mac, mac, 6);
  const int n = len > 4 ? 4 : len;
  memcpy(_ring[_tail].data, data, n);
  _ring[_tail].len = n;
  _ring[_tail].rssi = rssi;
  _tail = next;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void EspNowRx::onRxCb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!_self) return;
  int8_t rssi = 0;
  if (info && info->rx_ctrl) rssi = (int8_t)info->rx_ctrl->rssi;
  _self->pushRx(info ? info->src_addr : nullptr, data, len, rssi);
}
#else
void EspNowRx::onRxCb(const uint8_t *mac, const uint8_t *data, int len) {
  if (!_self) return;
  _self->pushRx(mac, data, len, 0);            // 旧内核回调无 RSSI
}
#endif

void EspNowRx::onTxCb(const uint8_t *mac, esp_now_send_status_t status) {
  if (!_self) return;
  if (status == ESP_NOW_SEND_SUCCESS) _self->_txOk++;
  else _self->_txFail++;
}
