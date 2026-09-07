#include "EspNowProbe.h"
#include "config.h"
#include "Trace.h"
#include <WiFi.h>
#include <string.h>

EspNowProbe *EspNowProbe::_self = nullptr;

static void printMac(const uint8_t *m) {
  DBG("%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static bool macIsBroadcast(const uint8_t *m) {
  for (int i = 0; i < 6; i++)
    if (m[i] != 0xFF) return false;
  return true;
}

// ======================= 初始化 =======================
void EspNowProbe::begin() {
  memset(&_peers, 0, sizeof(_peers));
  _peerN = 0;
  _self = this;
  _link = LINK_WAIT;
  _autoPairPending = false;

  WiFi.mode(WIFI_STA);                        // 仅 2.4G 协议栈，不连 AP
  delay(100);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t e = esp_now_init();
  _initOk = (e == ESP_OK);
  if (!_initOk) {
    DBG("#T3 esp_now_init failed: %s\n", esp_err_to_name(e));
    _link = LINK_INIT;
  }
  esp_now_register_recv_cb(rxCb);
  esp_now_register_send_cb(txCb);
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  _beginMs = millis();
  _lastHbMs = _lastStatusMs = _lastLinkMs = 0;

  // config.h 固定对端（非广播）
  const uint8_t cfgMac[6] = PEER_MAC;
  if (!macIsBroadcast(cfgMac)) {
    addPeer(cfgMac, true);
    DBG("#T3 peer_config=");
    printMac(cfgMac);
    DBG("\n");
  }

  DBG("#T3 espnow_init=%s ch=%u own_mac=", _initOk ? "OK" : "FAIL", ESPNOW_CHANNEL);
  printMac(mac);
  DBG(" (%s)\n", macIsBroadcast(cfgMac) ? "自动发现模式" : "固定对端模式");
}

uint32_t EspNowProbe::elapsedMs() const {
  return millis() - _beginMs;
}

// ======================= 对端表 =======================
EspNowProbe::Peer *EspNowProbe::findPeer(const uint8_t *m) {
  for (uint8_t i = 0; i < _peerN; i++)
    if (memcmp(_peers[i].mac, m, 6) == 0) return &_peers[i];
  return nullptr;
}

void EspNowProbe::addPeer(const uint8_t *m, bool cfg) {
  if (findPeer(m)) return;
  if (_peerN >= MAX_PEERS) {
    DBG("#T3 peer_full drop=");
    printMac(m);
    DBG("\n");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, m, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_err_t e = esp_now_add_peer(&peer);
  if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
    DBG("#T3 add_peer fail(%s) mac=", esp_err_to_name(e));
    printMac(m);
    DBG("\n");
    return;
  }
  Peer &p = _peers[_peerN++];
  memcpy(p.mac, m, 6);
  p.cfg = cfg;
  p.rx = p.txOk = p.txFail = 0;
  p.lastRxMs = p.lastTxMs = 0;
  DBG("#T3 %s peer=", cfg ? "use" : "auto_pair");
  printMac(m);
  DBG(" n=%u\n", _peerN);
}

void EspNowProbe::processAutoPair(uint32_t now) {
  (void)now;
  if (!_autoPairPending) return;
  _autoPairPending = false;
  if (memcmp(_autoPairMac, mac, 6) == 0) return;   // 自己发的不可能收到，防御
  if (!findPeer(_autoPairMac)) addPeer(_autoPairMac, false);
}

// ======================= 心跳发送 =======================
void EspNowProbe::sendHbBroadcast(uint32_t now) {
  (void)now;
  uint8_t buf[4] = { FRAME_MAGIC, FRAME_HB, (uint8_t)hbCount, 0 };
  const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  esp_now_send(bcast, buf, sizeof(buf));   // 广播无 ACK，仅用于发现
  hbCount++;
}

void EspNowProbe::sendHbUnicast(Peer &p, uint32_t now) {
  uint8_t buf[4] = { FRAME_MAGIC, FRAME_HB, (uint8_t)hbCount, 0 };
  esp_err_t e = esp_now_send(p.mac, buf, sizeof(buf));  // 单播 → 链路层 ACK
  p.lastTxMs = now;
  if (e != ESP_OK) {
    p.txFail++;
    txFailTotal++;
  }
  hbCount++;
}

// ======================= 接收 =======================
void EspNowProbe::handleRx(const uint8_t *m, const uint8_t *data, int len) {
  if (len < 4 || data[0] != FRAME_MAGIC) {
    rxBad++;
    if (verbose) {
      DBG("#RX bad len=%d first=0x%02X from=", len, len ? data[0] : 0);
      printMac(m);
      DBG("\n");
    }
    return;
  }
  rxTotal++;
  lastRxMs = millis();
  const uint8_t type = data[1];
  switch (type) {
    case FRAME_FIRE:  rxFire++;  break;
    case FRAME_HB:    rxHb++;    break;
    case FRAME_FAULT: rxFault++; break;
    case FRAME_POWER:
      rxPower++;
      if (onPowerFrame) onPowerFrame(data[2] != 0);
      break;
    default:          rxOther++; break;
  }

  Peer *p = findPeer(m);
  if (p) {
    p->rx++;
    p->lastRxMs = millis();
  } else {
    // 未知发送方的合法帧 → 请求自动配对（主循环执行）
    memcpy(_autoPairMac, m, 6);
    _autoPairPending = true;
  }
  if (verbose) {
    DBG("#RX type=0x%02X d0=%u d1=%u from=", type, data[2], data[3]);
    printMac(m);
    DBG("\n");
  }
}

// ======================= 发送 ACK =======================
void EspNowProbe::handleTx(const uint8_t *m, esp_now_send_status_t status) {
  if (macIsBroadcast(m)) return;              // 广播不统计
  Peer *p = findPeer(m);
  if (status == ESP_NOW_SEND_SUCCESS) {
    txOkTotal++;
    lastTxOkMs = millis();
    if (p) p->txOk++;
  } else {
    txFailTotal++;
    if (p) p->txFail++;
  }
}

// ======================= 状态判定 =======================
void EspNowProbe::updateLink(uint32_t now) {
  Link prev = _link;
  if (!_initOk) {
    _link = LINK_INIT;
  } else {
    // 活动窗口 8s：期间既无收到帧也无 ACK 成功 → 视为暂时失去对端
    bool fresh = (lastRxMs && now - lastRxMs < 8000) ||
                 (lastTxOkMs && now - lastTxOkMs < 8000);
    if (!fresh) {
      _link = LINK_WAIT;
    } else if (txOkTotal >= 1 && rxTotal >= 1) {
      _link = LINK_BIDIR;
      everConfirmed = true;
    } else if (txOkTotal >= 3) {
      _link = LINK_TX_ONLY;
    } else {
      _link = LINK_WAIT;
    }
  }
  if (_link != prev) {
    const char *t = _link == LINK_BIDIR   ? "双向链路确认 (PASS)"
                  : _link == LINK_TX_ONLY ? "仅 TX 被对端 ACK（单向）"
                  : _link == LINK_INIT    ? "esp_now 初始化失败"
                                          : "等待对端确认…";
    DBG("#LINK %s\n", t);
  }
}

// ======================= 状态打印 =======================
void EspNowProbe::printStatus() {
  DBG("#T3 uptime=%lus link=", (unsigned long)(elapsedMs() / 1000UL));
  switch (_link) {
    case LINK_BIDIR:   DBG("bidir(PASS)"); break;
    case LINK_TX_ONLY: DBG("tx_only");     break;
    case LINK_INIT:    DBG("init_fail");   break;
    default:           DBG("wait");        break;
  }
  DBG(" peers=%u tx_ok=%lu tx_fail=%lu rx_total=%lu [fire=%lu hb=%lu fault=%lu "
      "power=%lu other=%lu bad=%lu] hb_seq=%lu\n",
      _peerN, (unsigned long)txOkTotal, (unsigned long)txFailTotal,
      (unsigned long)rxTotal, (unsigned long)rxFire, (unsigned long)rxHb,
      (unsigned long)rxFault, (unsigned long)rxPower, (unsigned long)rxOther,
      (unsigned long)rxBad, (unsigned long)hbCount);
  for (uint8_t i = 0; i < _peerN; i++) {
    const Peer &p = _peers[i];
    DBG("#T3   peer%u %s mac=", (unsigned)i, p.cfg ? "[cfg]" : "[auto]");
    printMac(p.mac);
    DBG(" rx=%lu tx_ok=%lu tx_fail=%lu last_rx=%lus\n", (unsigned long)p.rx,
        (unsigned long)p.txOk, (unsigned long)p.txFail,
        (unsigned long)(p.lastRxMs ? (millis() - p.lastRxMs) / 1000UL : 0));
  }
}

// ======================= 主循环调用 =======================
void EspNowProbe::update(uint32_t now) {
  processAutoPair(now);

  if (_initOk) {
    // 心跳：有对端则逐对端单播；无对端则广播（邀请发现）
    if (now - _lastHbMs >= HB_INTERVAL_MS) {
      _lastHbMs = now;
      if (_peerN) {
        for (uint8_t i = 0; i < _peerN; i++) sendHbUnicast(_peers[i], now);
      } else {
        sendHbBroadcast(now);
      }
    }
    if (now - _lastLinkMs >= 500) {
      _lastLinkMs = now;
      updateLink(now);
    }
    if (now - _lastStatusMs >= LINK_STATUS_MS) {
      _lastStatusMs = now;
      printStatus();
    }
  }
}

void EspNowProbe::reset(uint32_t now) {
  for (uint8_t i = 0; i < _peerN; i++) {
    if (!_peers[i].cfg) esp_now_del_peer(_peers[i].mac);
  }
  // 保留 config 固定对端，重建表
  Peer keep[MAX_PEERS];
  uint8_t keepN = 0;
  for (uint8_t i = 0; i < _peerN; i++)
    if (_peers[i].cfg) keep[keepN++] = _peers[i];
  memcpy(_peers, keep, sizeof(Peer) * keepN);
  _peerN = keepN;
  rxTotal = txOkTotal = txFailTotal = 0;
  rxFire = rxHb = rxFault = rxPower = rxOther = rxBad = 0;
  lastRxMs = lastTxOkMs = 0;
  hbCount = 0;
  _autoPairPending = false;
  _lastHbMs = 0;
  _beginMs = now;
  _lastLinkMs = 0;
  _link = LINK_WAIT;
  everConfirmed = false;
  DBG("#T3 stats reset, keep cfg peers=%u\n", _peerN);
}

// ======================= 静态回调 =======================
void EspNowProbe::rxCb(const uint8_t *mac, const uint8_t *data, int len) {
  if (_self) _self->handleRx(mac, data, len);
}

void EspNowProbe::txCb(const uint8_t *mac, esp_now_send_status_t status) {
  if (_self) _self->handleTx(mac, status);
}
