#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>

// ============================================================
// ESP-NOW 自动连接（枪端自检 T3）
//
// 行为：
//   1) 开机优先连接「上次成功连接」的地址（NVS 持久化）；
//   2) 若 3s 内连不上 → 扫描附近 ESP-NOW 设备帧（promiscuous 旁路抓包，
//      只收 ToDS=0/FromDS=0 的直连数据帧 = ESP-NOW 设备互发帧），
//      按源 MAC 统计 RSSI → 选信号最强逐个探测，成功即保存地址。
//
// "连接成功"定义（与仓库其它 ESP-NOW 固件一致，core 2.x 回调无 RSSI）：
//   - 对该 MAC 单播探测得到链路层 ACK（ESP_NOW_SEND_SUCCESS），或
//   - 收到来自该 MAC 的任一 ESP-NOW 帧。
//
// 帧格式与仓库一致：{ 0xA5, type, d0, d1 }；探测 type=0x20（对端不识别会忽略，
// 不影响链路层 ACK；检测板/espnow-verify 均兼容）。信道 1。
// ============================================================

#define ES_FRAME_MAGIC 0xA5
#define ES_FRAME_PROBE 0x20
#define ES_CHANNEL 1

struct EsCandidate {
  uint8_t mac[6];
  int8_t rssi;          // 平均 RSSI
  uint16_t frames;      // 抓到的帧数
};

class EspNowAuto {
 public:
  // 结果码
  enum Code {
    RC_INIT_FAIL = -4,     // esp_now_init 失败
    RC_SCAN_NONE = -3,     // 没保存过地址，扫描也没听到任何设备帧
    RC_SCAN_FAIL = -2,     // 扫到候选但逐个探测全失败
    RC_RECONNECT_FAIL = -1,// 上次地址 3s 内未连上（随后尝试了扫描）
    RC_CONNECTED = 0       // 已连接
  };

  // 初始化 WiFi/ESP-NOW（幂等，仅一次）
  void begin(int channel = ES_CHANNEL);

  // 执行自动连接：结果码见 enum；成功时 peer() 有效且已写 NVS
  int run(unsigned long deadlineMs = 3000);

  const uint8_t *peer() const { return _peerValid ? _peer : nullptr; }
  int8_t peerRssi() const { return _peerRssi; }
  bool usedSavedPeer() const { return _usedSaved; }
  const uint8_t *savedMac() const { return _savedValid ? _saved : nullptr; }

  // 扫描候选信息（供打印）
  uint16_t candCount() const { return _candCount; }
  const EsCandidate *candidate(uint16_t i) const {
    return (i < _candCount) ? &_cand[i] : nullptr;
  }
  uint32_t ackOk() const { return _ackOk; }
  uint32_t rxTotal() const { return _rxTotal; }

  static void eraseSaved();            // 清 NVS（备用）

 private:
  static void onRxCb(const uint8_t *mac, const uint8_t *data, int len);
  static void onTxCb(const uint8_t *mac, esp_now_send_status_t status);
  static void onSniffCb(void *buf, wifi_promiscuous_pkt_type_t type);

  void loadSaved();
  void savePeer(const uint8_t mac[6]);

  bool probeOnce(const uint8_t mac[6], unsigned long windowMs);   // 探测一个 MAC
  int  scanNearby();                                               // 抓包 + 排序（结果进 _cand）
  void collectScan(int ms);                                        // promiscuous 采集
  void clearTarget();

  Preferences _prefs;
  uint8_t _saved[6]; bool _savedValid = false;
  uint8_t _peer[6];  bool _peerValid = false;
  int8_t  _peerRssi = 0;
  bool    _usedSaved = false;

  // 探测目标（回调里比对）
  uint8_t _tgt[6]; bool _tgtValid = false;
  volatile bool _ackOk = false;     // 目标 MAC 的 ACK
  volatile bool _rxPeer = false;    // 收到目标 MAC 的帧
  volatile uint32_t _rxTotal = 0;   // 测试期间收到的帧总数

  static const uint16_t kCandMax = 24;
  EsCandidate _cand[kCandMax];
  uint16_t _candCount = 0;

  static EspNowAuto *_self;
};
