#include "EspNowAuto.h"
#include <WiFi.h>

EspNowAuto *EspNowAuto::_self = nullptr;

// ------------------------------------------------------------------
// 回调
// ------------------------------------------------------------------
void EspNowAuto::onRxCb(const uint8_t *mac, const uint8_t *data, int len) {
  if (!_self || !mac || !data || len < 1) return;
  // 只统计本协议帧（magic 0xA5），识别对端是否在场
  if (data[0] == ES_FRAME_MAGIC) _self->_rxTotal++;
  if (_self->_tgtValid && memcmp(mac, _self->_tgt, 6) == 0) _self->_rxPeer = true;
}

void EspNowAuto::onTxCb(const uint8_t *mac, esp_now_send_status_t status) {
  if (!_self || !mac) return;
  if (_self->_tgtValid && memcmp(mac, _self->_tgt, 6) == 0 &&
      status == ESP_NOW_SEND_SUCCESS) {
    _self->_ackOk = true;
  }
}

// 旁路抓包：统计直连数据帧（ToDS=0/FromDS=0，即 ESP-NOW 类互发帧）的源 MAC 与 RSSI
void EspNowAuto::onSniffCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (!_self || !buf || type != WIFI_PKT_DATA) return;
  auto *p = (wifi_promiscuous_pkt_t *)buf;
  if (p->rx_ctrl.sig_len < 24) return;
  const uint8_t *h = p->payload;

  // 802.11 帧控制：type=数据(2)；ToDS/FromDS=0（直连/自组网）
  const uint16_t fc = (uint16_t)h[0] | ((uint16_t)h[1] << 8);
  if (((fc >> 2) & 0x3) != 2) return;
  if ((fc & (1 << 8)) || (fc & (1 << 9))) return;   // ToDS 或 FromDS 置位 → 跳过

  const uint8_t *sa = h + 10;                       // addr2 = 源 MAC
  if (sa[0] & 0x01) return;                         // 组播/广播源，跳过

  // 找/建候选
  EsCandidate *slot = nullptr;
  for (uint16_t i = 0; i < _self->_candCount; i++) {
    if (memcmp(_self->_cand[i].mac, sa, 6) == 0) { slot = &_self->_cand[i]; break; }
  }
  if (!slot) {
    if (_self->_candCount >= EspNowAuto::kCandMax) return;
    slot = &_self->_cand[_self->_candCount++];
    memcpy(slot->mac, sa, 6);
    slot->rssi = 0;
    slot->frames = 0;
  }
  slot->rssi = (int8_t)(((int16_t)slot->rssi * slot->frames + p->rx_ctrl.rssi) /
                        (slot->frames + 1));        // 递推平均
  slot->frames++;
}

// ------------------------------------------------------------------
// 初始化
// ------------------------------------------------------------------
void EspNowAuto::begin(int channel) {
  _self = this;
  loadSaved();

  WiFi.mode(WIFI_STA);                              // 仅初始化 2.4G 栈，不连接
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);                    // 不休眠，保证探测/ACK 及时
  esp_now_init();
  esp_now_register_recv_cb(onRxCb);
  esp_now_register_send_cb(onTxCb);
}

// ------------------------------------------------------------------
// 主流程：先连上次地址，失败则扫描选最强
// ------------------------------------------------------------------
int EspNowAuto::run(unsigned long deadlineMs) {
  _rxTotal = 0;
  _candCount = 0;

  // ---- 1) 上次成功地址（开机优先）----
  if (_savedValid) {
    _usedSaved = true;
    if (probeOnce(_saved, deadlineMs)) {           // deadlineMs 默认 3000
      memcpy(_peer, _saved, 6);
      _peerValid = true;
      savePeer(_peer);
      return RC_CONNECTED;
    }
    // 3s 内未连上 → 走扫描
  }

  // ---- 2) 扫描附近，选信号最强 ----
  int n = scanNearby();
  if (n <= 0) {
    return _savedValid ? RC_RECONNECT_FAIL : RC_SCAN_NONE;
  }

  for (uint16_t i = 0; i < (uint16_t)n && i < 4; i++) {
    if (probeOnce(_cand[i].mac, 900)) {
      memcpy(_peer, _cand[i].mac, 6);
      _peerRssi = _cand[i].rssi;
      _peerValid = true;
      savePeer(_peer);
      return RC_CONNECTED;
    }
  }
  return RC_SCAN_FAIL;
}

// ------------------------------------------------------------------
// 扫描：promiscuous 抓包 collectScan，然后按 RSSI 排序
// ------------------------------------------------------------------
int EspNowAuto::scanNearby() {
  _candCount = 0;
  collectScan(1600);                                 // 1.6s 抓包窗口

  // 按平均 RSSI 降序（帧数多者优先）
  for (uint16_t i = 1; i < _candCount; i++) {
    EsCandidate key = _cand[i];
    uint16_t j = i;
    while (j > 0 && _cand[j - 1].rssi < key.rssi) { _cand[j] = _cand[j - 1]; j--; }
    _cand[j] = key;
  }
  return (int)_candCount;
}

void EspNowAuto::collectScan(int ms) {
  esp_wifi_set_promiscuous_rx_cb(onSniffCb);
  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;    // 全收，回调内自行过滤
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous(true);

  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)ms) delay(20);    // 采集窗口

  esp_wifi_set_promiscuous(false);
}

// ------------------------------------------------------------------
// 探测单个 MAC：加入 peer，窗口内发探测帧，等 ACK 或收到它的帧
// ------------------------------------------------------------------
bool EspNowAuto::probeOnce(const uint8_t mac[6], unsigned long windowMs) {
  // 加入 peer（已存在则报 ESP_ERR_ESPNOW_EXIST，同样视为就绪）
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ES_CHANNEL;
  peer.encrypt = false;
  esp_err_t e = esp_now_add_peer(&peer);
  if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) return false;

  _tgtValid = true;
  memcpy(_tgt, mac, 6);
  _ackOk = false;
  _rxPeer = false;

  uint8_t probe[4] = { ES_FRAME_MAGIC, ES_FRAME_PROBE, 0x00, 0x00 };
  const uint32_t t0 = millis();
  uint32_t lastSend = 0;
  while (millis() - t0 < windowMs) {
    if (_ackOk || _rxPeer) break;
    const uint32_t nowMs = millis();
    if (nowMs - lastSend >= 300) {
      lastSend = nowMs;
      probe[2] = (uint8_t)(nowMs >> 7);
      esp_now_send(mac, probe, sizeof(probe));
    }
    delay(30);
  }
  const bool ok = _ackOk || _rxPeer;
  _tgtValid = false;
  if (!ok) esp_now_del_peer(mac);                    // 失败就腾出 peer 表
  return ok;
}

// ------------------------------------------------------------------
// NVS 持久化
// ------------------------------------------------------------------
void EspNowAuto::loadSaved() {
  _savedValid = false;
  _prefs.begin("gun-st", true);                      // RO 打开读
  if (_prefs.isKey("peer") && _prefs.getBytes("peer", _saved, 6) == 6) {
    bool ok = false;
    for (int i = 0; i < 6; i++) if (_saved[i] != 0xFF && _saved[i] != 0x00) ok = true;
    _savedValid = ok;                                 // 过滤全 0 / 全 FF 空槽
  }
  _prefs.end();
}

void EspNowAuto::savePeer(const uint8_t mac[6]) {
  _prefs.begin("gun-st", false);
  _prefs.putBytes("peer", mac, 6);
  _prefs.end();
}

void EspNowAuto::eraseSaved() {
  Preferences p;
  p.begin("gun-st", false);
  p.remove("peer");
  p.end();
}
