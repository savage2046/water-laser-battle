#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"

// ============================================================
// ESP-NOW 连接自检（T3）—— triger-sensor 正式板
//
// 判定逻辑（帧协议与正式固件一致：{0xA5, type, d0, d1}，信道 1）：
//   1) 向已配对对端单播心跳 → ESP-NOW 链路层 ACK = 本机 TX 射频工作 +
//      对端同信道在场并收到（对端可为：枪端主控正式固件 / espnow-verify /
//      另一块本自检固件板）。
//   2) 收到对端任何合法帧（心跳/断电指令…）= 本机 RX 工作。
//   → tx ACK 且 rx 有帧 = 双向链路 PASS；仅 tx ACK = 单向（本机 RX 未验证）。
//
// 配对：
//   - config.h 的 PEER_MAC 非广播 → 开机直接单播配对（espnow-verify 伪装成
//     GUN_MAC 时填 GUN_MAC）。
//   - 否则纯自动发现：无对端时广播心跳，收到任意合法帧后把发送方自动加为
//     单播对端（同一固件两块板可互相对测）。
// ============================================================

class EspNowProbe {
 public:
  struct Peer {
    uint8_t mac[6];
    bool cfg;                 // 来自 config.h 的固定对端
    volatile uint32_t rx;     // 收到该对端帧数
    volatile uint32_t txOk;   // 单播心跳 ACK 成功
    volatile uint32_t txFail; // 单播心跳 ACK 失败
    volatile uint32_t lastRxMs;
    volatile uint32_t lastTxMs;
  };

  // 链路状态
  enum Link {
    LINK_INIT = 0,   // esp_now 初始化失败
    LINK_WAIT,       // 观察期内等待对端确认
    LINK_TX_ONLY,    // 仅 TX 被对端 ACK（对端不回传）
    LINK_BIDIR       // TX ACK + 收到对端帧（双向）
  };

  void begin();                // 初始化 WiFi/esp_now/配对，填本机 MAC
  bool initOk() const { return _initOk; }
  void update(uint32_t now);   // 心跳发送 / 自动配对 / 状态打印 / 状态判定
  void printStatus();          // 详细状态（含 peer 表）
  void reset(uint32_t now);    // 清统计 + 删除自动配对（保留 config 对端）
  bool verbose = false;        // 每帧打印开关

  // FRAME_POWER（对端下行断电/恢复）上抛，由主程序执行 MOSFET 动作
  void (*onPowerFrame)(bool on) = nullptr;

  // ---- 统计（回调上下文更新，主循环只读）----
  uint8_t mac[6];                              // 本机 MAC
  volatile uint32_t rxTotal = 0;               // 收到的本协议合法帧
  volatile uint32_t txOkTotal = 0;             // 单播 ACK 成功总数
  volatile uint32_t txFailTotal = 0;
  volatile uint32_t lastRxMs = 0;              // 最近一次收到帧的时刻
  volatile uint32_t lastTxOkMs = 0;            // 最近一次单播 ACK 成功时刻
  volatile uint32_t rxFire = 0, rxHb = 0, rxFault = 0, rxPower = 0, rxOther = 0;
  volatile uint32_t rxBad = 0;                 // 非本协议/短帧（仅统计，不配对）
  uint32_t hbCount = 0;                        // 心跳序号
  bool everConfirmed = false;                  // 是否曾达到过双向确认

  Link link() const { return _link; }
  const Peer *peer(uint8_t i) const { return i < _peerN ? &_peers[i] : nullptr; }
  uint8_t peerCount() const { return _peerN; }
  uint32_t elapsedMs() const;

  static EspNowProbe *_self;

 private:
  Peer _peers[MAX_PEERS];
  uint8_t _peerN = 0;
  bool _initOk = false;
  Link _link = LINK_INIT;
  uint32_t _beginMs = 0, _lastHbMs = 0, _lastStatusMs = 0, _lastLinkMs = 0;

  // 回调上下文 → 主循环处理的自动配对请求
  volatile bool _autoPairPending = false;
  uint8_t _autoPairMac[6];

  Peer *findPeer(const uint8_t *mac);
  void addPeer(const uint8_t *mac, bool cfg);
  void sendHbBroadcast(uint32_t now);
  void sendHbUnicast(Peer &p, uint32_t now);
  void handleRx(const uint8_t *mac, const uint8_t *data, int len);
  void handleTx(const uint8_t *mac, esp_now_send_status_t status);
  void updateLink(uint32_t now);
  void processAutoPair(uint32_t now);

  static void rxCb(const uint8_t *mac, const uint8_t *data, int len);
  static void txCb(const uint8_t *mac, esp_now_send_status_t status);
};
