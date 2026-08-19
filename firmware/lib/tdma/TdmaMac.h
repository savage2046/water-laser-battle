#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <RadioLib.h>
#include "TdmaProto.h"

// ============================================================
// TDMA MAC（自适应时隙 + 开机自动分配信道，见 docs/tdma-mac.md）
//
// 超帧（每信道独立）：
//   信标窗 10ms + 广播下行窗 10ms + N×设备时隙 10ms + 注册时隙 10ms
//   N = 该信道活跃设备数（自适应，1..17）→ 超帧 = 30 + 10N ms
//   N=1  → 40ms（最坏时延 40ms）；N=17 → 200ms
//
// 开机自动分配信道：
//   设备扫描信道表（听信标）→ 注册时隙发 JOIN → 网关回 TF_ASSIGN
//   （信道号 + 时隙号 + 当前 N + mapVer）→ 设备跳到分配信道按时隙发射
//
// 时隙自适应：
//   网关按注册序密集重排（时隙压缩），mapVer 递增，信标广播 (mapVer, N)；
//   设备 mapVer 失配或 slot≥N 时转入注册时隙补 JOIN，避免碰撞
//
// 运行于独立 FreeRTOS 任务（core 1，优先级 5）；接收 1ms 间隔轮询，
// TX 时刻 micros() 忙等精确定时；所有配置参数与 docs/tdma-mac.md 一致
// ============================================================

class TdmaMac {
 public:
  enum Role { ROLE_DEVICE = 0, ROLE_GATEWAY = 1 };

  // devIdx: 设备短号 0-255（网关传 0xFF）
  // r: RadioLib SX1262 实例（RadioLink::getRadio()）
  // channels: 信道频率表（MHz，如 {470.0, 471.0, ...}）
  // channelCount: 信道数；myChannel: 网关固定信道索引（设备=0，扫描决定）
  // maxSlots: 每信道最大设备时隙（T1=17）
  void begin(Role role, uint8_t devIdx, SX1262 *r, const float *channels,
             uint8_t channelCount, uint8_t myChannel, uint8_t maxSlots);

  // ---- 设备侧 ----
  // 排队一帧在自身时隙发射（payload5 可 NULL）。返回 false=队列满
  bool send(uint8_t type, const uint8_t *payload5, uint8_t flags, uint16_t seq);
  // 取广播窗收到的下行帧。返回 false=无
  bool pollDownlink(TdmaFrame &out);
  bool assigned() { return _assigned; }  // 已获信道+时隙分配
  uint8_t channelIdx() { return _channel; }
  uint8_t slotIdx() { return _slot; }
  // 注册帧内容（设备开机自动 JOIN 用）：payload5=5B deviceId，flags=TF_FLAG_HELMET
  void setJoinPayload(const uint8_t payload5[5], uint8_t flags);

  // ---- 网关侧 ----
  // 排队下行帧（广播窗发射）。target=TF_BROADCAST_IDX 或具体 devIdx
  bool sendDownlink(uint8_t type, const uint8_t *payload5, uint8_t flags,
                    uint8_t target, uint16_t seq);
  // 取各上行时隙/注册时隙收到的帧。返回 false=无
  bool pollUplink(TdmaFrame &out);
  uint8_t activeCount() { return _gwCount; }  // 本信道活跃设备数

  bool locked() { return _locked; }   // 已锁信标相位
  bool started() { return _task != nullptr; }  // MAC 任务已创建（多射频下可能失败）

 private:
  static void taskEntry(void *arg);
  void runDevice();
  void runGateway();
  void waitUntil(int32_t targetUs);
  bool readPacketPoll(TdmaFrame &out, uint32_t deadlineUs);
  void txFrame(const TdmaFrame &f);
  void lockFromBeacon(const TdmaFrame &f, uint32_t rxEndUs);
  void pushRx(const TdmaFrame &f);
  void pushDl(const TdmaFrame &f);

  // 设备侧
  bool scanForBeacon();          // 扫描信道表找信标
  void tryJoin(uint32_t regStartUs);  // 注册时隙发 JOIN（带随机退避）

  // 网关侧
  void onGwUplink(const TdmaFrame &f);  // 维护设备表/触发重排
  void reSlot();                        // 密集重排 + 队列 TF_ASSIGN
  void expireDevices();                 // 心跳超时清理

  Role _role;
  uint8_t _devIdx;
  SX1262 *_r = nullptr;
  const float *_channels = nullptr;
  uint8_t _channelCount = 0;
  uint8_t _maxSlots = 0;
  TaskHandle_t _task = nullptr;

  volatile bool _locked = false;   // 已锁信标相位
  volatile bool _assigned = false; // 设备已获 (信道, 时隙)
  volatile bool _armed = false;    // 网关：首个超帧栅格已排定
  uint8_t _channel = 0;            // 设备当前信道索引
  uint8_t _slot = 0;               // 设备时隙索引
  uint8_t _mapVer = 0;             // 设备已分配的 mapVer
  uint8_t _beaconMapVer = 0;       // 信标广播的 mapVer
  uint8_t _joinTries = 0;
  uint32_t _lastJoinUs = 0;
  uint8_t _joinPayload[5];         // 注册帧 payload（5B deviceId）
  uint8_t _joinFlags = 0;          // 注册帧 flags（如 TF_FLAG_HELMET）

  uint32_t _sfCounter = 0;
  uint32_t _sfStartUs = 0;         // 下一超帧起点
  uint8_t _n = 1;                  // 当前 N（来自信标/网关本地）

  // 网关设备表（每信道）
  struct GwDev {
    uint8_t idx;
    uint8_t slot;
    uint32_t lastSeenMs;
    bool valid;
  };
  GwDev _gw[17];
  uint8_t _gwCount = 0;

  // 环形队列（单生产者/单消费者 + 互斥锁；cap 必须为 2 的幂）
  TdmaFrame _tx[8];
  volatile uint8_t _txHead = 0, _txTail = 0;
  TdmaFrame _dl[32];   // 下行队列（重排 ASSIGN 突发：最多 17 条 + 常规下行）
  volatile uint8_t _dlHead = 0, _dlTail = 0;
  TdmaFrame _rx[16];
  volatile uint8_t _rxHead = 0, _rxTail = 0;
  SemaphoreHandle_t _mutex = nullptr;
};

extern TdmaMac tdma;
