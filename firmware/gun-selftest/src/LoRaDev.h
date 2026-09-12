#pragma once
#include <Arduino.h>
#include <RadioLib.h>
#include "config.h"
#include "TdmaProto.h"

// ============================================================================
// LoRa 网关自动连接 + 1s 心跳（T4）
//
// 设备侧 TDMA（与 firmware/lib/tdma 的 TdmaMac 设备态同语义）：
//   1) 开机 / 掉线后：按 470.0 + k×2.0MHz 扫描 20 个标准频点（每点 250ms），
//      找网关信标 TF_BEACON；若多个信标，选 N（载荷）最小者 = 负载均衡；
//   2) 锁相：以信标空口起点为超帧基准（与网关同一点）；
//   3) 注册：在注册时隙发 JOIN（payload = "G0001"）；网关回 TF_ASSIGN
//      （信道/时隙/N/mapVer）；slot=0xFF 表示该信道满 → 重新扫频；
//   4) 心跳：**每 1000ms 入队一个 TF_HB**，在自身时隙内发出（每超帧最多一帧）；
//   5) 掉线：5s 没听到信标 → 判掉线，回到 1)。
//
// 实现要点（与正式 TdmaMac 的差异，见 README §5）：
//   - 发射走 startTransmit + 轮询 IRQ 寄存器（DIO1 未接线，阻塞 transmit 会白等 46ms）
//   - 接收全程保持连续 RX（不在窗口间 standby）：抗光路矫正任务抢占，
//     任一时刻收到信标都能重新锁相，不会失锁
//   - 不严格丢弃"迟到"的心跳：照发并统计迟到次数（网关端会看到缺口）
// ============================================================================

struct LoRaPkt {
  TdmaFrame f;
  bool      tdmaOk;
  int16_t   rssi;
  int8_t    snrX4;
  int16_t   feHz;
  uint32_t  startUs;   // 估算空口起点 = 到达时刻 − TDMA_AIR_US
  uint32_t  ms;
};

// 链路状态（供报告打印）
struct DevLink {
  bool     ready = false;
  bool     locked = false;      // 已锁定网关信标
  bool     assigned = false;    // 已获信道 + 时隙
  uint8_t  grid = 0;            // 当前频点栅格 k
  uint8_t  slot = 0xFF;
  uint8_t  n = 1;               // 网关当前设备数（信标载荷）
  uint8_t  mapVer = 0;
  uint8_t  beaconMapVer = 0;
  uint8_t  dlySlot = 0;         // ASSIGN 给出的时隙
  uint8_t  dlyN = 0;
  uint8_t  dlyMapVer = 0;

  int16_t  beaconRssi = 0;
  int8_t   beaconSnrX4 = 0;
  int16_t  beaconFeHz = 0;
  uint32_t lastBeaconMs = 0;

  uint32_t beacons = 0;
  uint32_t scans = 0;
  uint32_t joins = 0;
  uint32_t assigns = 0;
  uint32_t dlRx = 0;
  uint32_t rxBad = 0;
  uint32_t txFail = 0;
  uint32_t airUsLast = 0;
  uint32_t regLatMs = 0;
  uint32_t connectMs = 0;       // 首次获分配的时刻（millis）

  uint32_t hbQueued = 0;
  uint32_t hbTx = 0;
  uint32_t hbLate = 0;
  uint32_t lastHbMs = 0;
  uint32_t hbIntMin = 0, hbIntMax = 0, hbIntSum = 0;
  uint16_t hbIntN = 0;
  int32_t  hbOffMin = 0, hbOffMax = 0, hbOffSum = 0;
  uint16_t hbOffN = 0;

  int16_t  beginCode = RADIOLIB_ERR_UNKNOWN;
};

class LoRaDev {
 public:
  bool begin();          // SPI + Module + SX1262 begin + 版本寄存器
  void start();          // 建任务（core1 优先级 5）

  const DevLink &link() const { return _st; }
  const uint8_t *versionReg() const { return _ver; }
  const char *chipClass() const { return _chipClass; }   // 命中的 RadioLib 类名
  uint32_t theoreticalAirUs() const { return _airTheo; }
  uint32_t irqCount() const { return _irqCount; }   // DIO1 中断次数（G04 是否真在跳变）

  void requestRescan();               // 重新扫网关
  void setGridManual(uint8_t k);      // 手动锁定频点（跳过扫描）
  void sendJoinNow() { _forceJoin = true; }

 private:
  static void taskEntry(void *arg);
  static void IRAM_ATTR dio1Isr();    // DIO1 上升沿（RX_DONE）→ 记录到达时刻
  void run();
  bool scanGateway();                 // 扫 20 点找信标（成功返回 true）
  bool tryGrid(uint8_t k);            // 在频点 k 驻留听信标
  void runLocked();                   // 锁定后的超帧循环
  bool pollRx(LoRaPkt &out);
  void onRx(const LoRaPkt &p);
  void txHeartbeat(uint32_t slotAtUs);
  void txJoin(uint32_t atUs);
  int16_t txFrame(uint8_t type, const uint8_t *payload5, uint8_t flags,
                  uint32_t *startUs, uint32_t *airUs);
  void applyGrid(uint8_t k);

  SX126x  *_r = nullptr;      // 基类指针：SX1262/SX1268/LLCC68 自动探测后赋值
  Module  *_mod = nullptr;
  const char *_chipClass = "?";   // 命中的 RadioLib 类名（报告用）
  bool     _ready = false;
  uint8_t  _ver[16] = {0};
  uint32_t _airTheo = 0;

  DevLink  _st;
  volatile bool _needRescan = false;
  volatile bool _forceJoin = false;
  volatile bool _manualGrid = false;
  volatile uint8_t _manualK = 0;

  // 超帧状态
  uint32_t _sfPhaseUs = 0;      // 本超帧信标空口起点
  uint32_t _sfCounter = 0;
  uint8_t  _n = 1;              // 信标里的 N（决定注册槽位置）
  bool     _sfSlotSent = false;
  bool     _sfRegSent = false;
  bool     _sfJoinDue = true;   // 本超帧是否轮到发 JOIN（JOIN_EVERY_SF 节流）
  uint32_t _sfJoinSeq = 0;
  uint32_t _joinJitterUs = 0;
  uint32_t _lastHbEnqMs = 0;
  uint8_t  _hbPending = 0;
  uint16_t _seq = 0;            // 上行帧序号（网关按 (deviceId, seq) 去重）
  uint32_t _joinSentMs = 0;
  int8_t   _lastGoodGrid = -1;

  // ---- DIO1 中断（G04）----
  // 上升沿 = RX_DONE → ISR 记 micros()，得到 ±µs 的到达时刻。
  // 本固件的"信标相位 _sfPhaseUs = 到达时刻 − 空口时长"与"时隙对齐偏移"
  // 都直接依赖这个精度（轮询方式在 vTaskDelay 轮询下误差可达 ~1ms）。
  static LoRaDev   *_self;
  volatile uint32_t _rxIrqUs = 0;
  volatile bool     _rxIrqLatched = false;
  volatile bool     _txWindow = false;   // TX 期间的 DIO1 跳变（TX_DONE）不计入
  volatile uint32_t _irqCount = 0;

  TaskHandle_t _task = nullptr;
};

extern LoRaDev g_dev;

// main.cpp 提供：USB CDC 门控打印（无主机时静默）
void dsLog(const char *fmt, ...);
