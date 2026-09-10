#pragma once
#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "Stats.h"
#include "TdmaProto.h"

// ============================================================================
// lora-gwtest 射频引擎（跑在独立 FreeRTOS 任务：core1，优先级 5）
//
// 五种模式（串口命令 m 切换）：
//   SCAN  20 个标准栅格频点逐个测底噪 + 驻留收帧 → 找出枪端/网关在哪个信道
//   MON   锁定单频点被动监听（绝不发射）→ 只统计，用来验证另一块 PCB 的射频
//   GW    TDMA 网关（与 TdmaMac::runGateway 同结构）：发信标 + 下行窗 +
//         N×设备时隙 + 注册时隙；真实枪端会扫到本信标并 JOIN 上来，
//         从而在「正式固件 ↔ 本板」之间跑通完整 TDMA 链路并统计全部指标
//   PING  主动 ping（测试帧 0x1E）→ 等 ECHO 应答（0x1F），统计 RTT/PER
//   ECHO  应答器：收到 ping 立即回 pong（用于两块本固件板互测真 RTT）
//
// ⚠️ DIO1 未接线（gun-board 主控页）：不能用 RadioLib 阻塞 transmit()
//    （它靠读 DIO1 电平判完成，未接线时必然超时 ~46ms），本固件改为
//    startTransmit() + 轮询 IRQ 寄存器等 TX_DONE（见 txRaw）。
//    接收同理：轮询 IRQ 寄存器（RX_DONE/CRC_ERR/HEADER_ERR）而不是轮询电平。
// ============================================================================

enum TestMode {
  MODE_SCAN = 0,
  MODE_MON,
  MODE_GW,
  MODE_PING,
  MODE_ECHO,
  MODE_COUNT
};

// 一帧的观测记录（射频任务 → 打印任务）
struct RxEvt {
  uint32_t  us;       // 到达时刻（micros）
  uint32_t  ms;       // 到达时刻（millis）
  int16_t   rssi;     // 包 RSSI（dBm，取整）
  int8_t    snrX4;    // SNR×4 定点
  uint16_t  len;      // 空口长度
  int16_t   freqErrHz;// 该包的载频偏差（Hz；RadioLib getFrequencyError）
  bool      crcOk;    // LoRa 硬件 CRC/报头通过
  bool      tdmaOk;   // len==10 且 CRC-8 通过
  uint8_t   grid;     // 栅格索引
  bool      slotFrame;// GW 模式：落在设备时隙内（offUs/respUs 有效）
  int32_t   offUs;    // 发射起点相对期望时隙起点的偏移
  uint32_t  respUs;   // 相对本超帧信标起点的时延
  uint8_t   n;        // raw 有效字节数
  uint8_t   raw[16];
  TdmaFrame f;        // tdmaOk 时有效
};

class LoRaTest {
 public:
  // ---- 初始化 ----
  bool begin();                 // SPI + 模块在位检查 + RadioLib begin
  void start();                 // 创建射频任务

  bool      ready() const { return _ready; }
  int16_t   beginCode() const { return _beginCode; }
  const char *chipName() const { return _chip; }
  const char *chipClass() const { return _chipClass; }   // 命中的 RadioLib 类名
  const uint8_t *versionReg() const { return _ver; }  // 0x0320 版本字符串（16B，ASCII）
  uint32_t  theoreticalAirUs() const { return _airTheo; }  // getTimeOnAir(10)

  // ---- 模式 / 频点 ----
  void      setMode(TestMode m);
  TestMode  mode() const { return _mode; }
  static const char *modeName(TestMode m);
  void      setGrid(uint8_t k);          // 0..19 → 470.0 + k×2.0 MHz
  uint8_t   grid() const { return _grid; }
  void      rescan();                    // 触发重新扫描 / 打断当前循环
  void      sendPing(uint16_t count);    // 0xFFFF = 持续 ping
  void      broadcastStart();            // 广播 TF_START（可选：促使枪端活跃）
  void      broadcastEnd();              // 广播 TF_END

  // ---- GW 模式状态 ----
  uint8_t   devCount() const { return _gwCount; }
  uint8_t   mapVer() const { return _mapVer; }
  uint8_t   n() const { return _n; }
  uint32_t  sfMs() const { return 30u + 10u * (uint32_t)_n; }
  int8_t    slotOf(uint8_t idx) const;
  bool      scanActive() const { return _scanActive; }
  bool      busyTx() const { return _busyTx; }
  uint32_t  scanPassDone() const { return _scanPassDone; }
  float     idleRssi() const { return _idleRssi; }
  uint32_t  irqCount() const { return _irqCount; }   // DIO1 中断次数（G04 是否真的在跳变）

  // ---- 事件 / 提示 ----
  bool      popEvt(RxEvt &out);
  bool      popNote(char *out, size_t n);
  uint32_t  evtDropped() const;

 private:
  static void taskEntry(void *arg);
  static void IRAM_ATTR dio1Isr();   // DIO1 上升沿（RX_DONE）→ 记录到达时刻
  void run();
  void runScan();
  void runMon();
  void runGw();
  void runPing();
  void runEcho();

  bool    pollRx(RxEvt &out);
  bool    pollRxUntil(RxEvt &out, uint32_t deadlineUs);   // 忙等（≤12ms）
  bool    pollRxBusy(RxEvt &out, uint32_t busyUs);        // 忙等固定时长
  int16_t txRaw(const uint8_t *buf, size_t len, uint32_t *startUs,
                uint32_t *airUs);
  int16_t txFrame(const TdmaFrame &f, uint32_t *startUs, uint32_t *airUs);
  void    applyGrid(uint8_t k, bool force);
  void    handleBcReq();
  void    pushEvt(const RxEvt &e);
  void    note(const char *fmt, ...);
  void    onFrameStats(const RxEvt &e, bool slotFrame, int32_t offUs,
                       uint32_t respUs);
  // GW
  int8_t  gwFind(uint8_t idx) const;
  void    gwOnUplink(const RxEvt &e, bool regSlot);
  void    gwReSlot();
  void    gwExpire();
  bool    dlPush(const TdmaFrame &f);
  bool    dlPop(TdmaFrame &out);
  // PING/ECHO
  void    pingOnRx(const RxEvt &e);
  void    echoOnRx(const RxEvt &e);

  SX126x  *_r = nullptr;      // 基类指针：SX1262/SX1268/LLCC68 自动探测后赋值
  Module  *_mod = nullptr;
  const char *_chipClass = "?";   // 命中的 RadioLib 类名（报告用）
  bool     _ready = false;  int16_t  _beginCode = RADIOLIB_ERR_UNKNOWN;
  char     _chip[24];
  uint8_t  _ver[16];
  uint32_t _airTheo = 0;

  volatile TestMode _mode = MODE_SCAN;
  volatile uint8_t  _grid = 0;
  volatile uint32_t _gridReq = 0;      // setGrid 递增；射频任务比对 _gridApplied 后应用
  uint32_t _gridApplied = 0;
  volatile bool     _rescan = false;
  volatile bool     _scanActive = false;
  volatile bool     _busyTx = false;
  volatile uint32_t _scanPassDone = 0;
  volatile float    _idleRssi = 0;
  volatile uint16_t _pingBatch = 0;
  volatile uint32_t _bcStartReq = 0;   // 1=START 2=END
  uint16_t _pingSeq = 0;

  // ---- DIO1 中断（G04）----
  // 上升沿 = RX_DONE（startReceive 的 DIO1 掩码）：在 ISR 里记 micros()，
  // 得到 ±µs 的到达时刻，替代"轮询到包的时刻"（后者有 50µs~1ms 不等的延迟）。
  // TX 期间的 DIO1 跳变（TX_DONE）用 _txWindow 排除。
  static LoRaTest  *_self;
  volatile uint32_t _rxIrqUs = 0;
  volatile bool     _rxIrqLatched = false;
  volatile bool     _txWindow = false;
  volatile uint32_t _irqCount = 0;

  TaskHandle_t _task = nullptr;

  // GW 设备表（本信道）
  struct GwDev { uint8_t idx; uint8_t slot; uint32_t lastSeenMs; bool valid; };
  GwDev    _gw[TDMA_MAX_SLOTS];
  uint8_t  _gwCount = 0;
  uint8_t  _mapVer = 0;
  uint8_t  _n = 1;
  uint32_t _sfCounter = 0;
  uint32_t _sfStartUs = 0;
  uint32_t _lastBcStart = 0;   // 上一超帧的信标空口起点（测实测周期）
  bool     _armed = false;

  // 下行队列（环形；互斥保护，主任务可推送）
  TdmaFrame _dl[TDMA_MAX_SLOTS + 8];
  volatile uint8_t _dlHead = 0, _dlTail = 0;
  SemaphoreHandle_t _dlMtx = nullptr;

  // 事件环形 + 提示环形
  RxEvt    _evt[EVT_RING];
  volatile uint8_t _evtHead = 0, _evtTail = 0;
  char     _note[NOTE_RING][NOTE_LEN];
  volatile uint8_t _noteHead = 0, _noteTail = 0;
};

extern LoRaTest g_test;

// main.cpp 提供：USB CDC 门控打印（无主机时静默、不阻塞）
void gwLog(const char *fmt, ...);
