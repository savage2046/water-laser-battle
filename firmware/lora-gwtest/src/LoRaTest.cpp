#include "LoRaTest.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>

// ============================================================================
// 射频引擎实现（见 LoRaTest.h 的架构说明）
// ============================================================================

LoRaTest g_test;
LoRaTest *LoRaTest::_self = nullptr;

// ---------------------------------------------------------------- DIO1 中断
// G04 = SX1268 DIO1。startReceive() 把 DIO1 掩码设成 RX_DONE（+超时），
// 所以上升沿 = "收到一个包"；在 ISR 里记 micros() 即得到 ±µs 的到达时刻。
// TX 期间（startTransmit 会把掩码改成 TX_DONE）用 _txWindow 屏蔽掉。
void IRAM_ATTR LoRaTest::dio1Isr() {
  LoRaTest *s = _self;
  if (s == nullptr || s->_txWindow) return;
  s->_irqCount++;
  s->_rxIrqUs = micros();
  s->_rxIrqLatched = true;
}

// ---------------------------------------------------------------- 基础工具
void LoRaTest::pushEvt(const RxEvt &e) {
  const uint8_t next = (uint8_t)((_evtTail + 1) & (EVT_RING - 1));
  if (next == _evtHead) {           // 满：丢弃本帧（生产者不碰 head，避免双写竞态）
    statsG().evtDropped++;
    return;
  }
  _evt[_evtTail] = e;
  _evtTail = next;
}

bool LoRaTest::popEvt(RxEvt &out) {
  if (_evtHead == _evtTail) return false;
  out = _evt[_evtHead];
  _evtHead = (uint8_t)((_evtHead + 1) & (EVT_RING - 1));
  return true;
}

uint32_t LoRaTest::evtDropped() const { return statsG().evtDropped; }

void LoRaTest::note(const char *fmt, ...) {
  char buf[NOTE_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  const uint8_t next = (uint8_t)((_noteTail + 1) % NOTE_RING);
  if (next == _noteHead) {
    _noteHead = (uint8_t)((_noteHead + 1) % NOTE_RING);
  }
  strncpy(_note[_noteTail], buf, NOTE_LEN - 1);
  _note[_noteTail][NOTE_LEN - 1] = 0;
  _noteTail = next;
}

bool LoRaTest::popNote(char *out, size_t n) {
  if (_noteHead == _noteTail) return false;
  strncpy(out, _note[_noteHead], n - 1);
  out[n - 1] = 0;
  _noteHead = (uint8_t)((_noteHead + 1) % NOTE_RING);
  return true;
}

// ---------------------------------------------------------------- 初始化
bool LoRaTest::begin() {
  SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);

  _mod = new Module(PIN_SX_NSS, PIN_SX_DIO1, PIN_SX_RST, PIN_SX_BUSY);

  // ---- 自动探测芯片类型（关键！）----
  // RadioLib 的 SX126x 家族类用**版本字符串**做身份校验（findChip → strncmp 6 字节）：
  //   SX1262 类只认 "SX1261"（官方注释：所有 SX1262 都上报 SX1261）
  //   SX1268 类只认 "SX1268"、LLCC68 类只认 "LLCC68"
  // 拿错类 → reset + 读版本重试 10 次（约 100ms）后返回 -2 CHIP_NOT_FOUND，
  // **看起来像"芯片无应答"的硬件故障，其实焊接全好**。
  // U2 = Ra-01S（SX1268）→ 早期固件用 SX1262 类，因此 LoRa 从未真正跑起来。
  // 这里依次试，并把命中的类和实际版本字符串打出来，彻底消除这个猜测。
  static const char *kNames[3] = { "SX1262(认 SX1261)", "SX1268", "LLCC68" };
  int16_t codes[3] = { 0, 0, 0 };
  _beginCode = RADIOLIB_ERR_CHIP_NOT_FOUND;
  for (uint8_t t = 0; t < 3; t++) {
    // 注意：LoRa 的 begin(freq,bw,sf,cr,sync,pwr,preamble,tcxo) 只在**派生类**上声明，
    //       基类 SX126x 只有 GFSK 版 —— 所以要用具体类型指针调 begin()，成功后再上转为 _r。
    SX126x *cand = nullptr;
    switch (t) {
      case 0: {
        SX1262 *p = new SX1262(_mod);
        codes[t] = p->begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                            RADIO_SYNC_WORD, RADIO_TX_POWER_DBM,
                            RADIO_PREAMBLE_LEN, 0);
        cand = p;
        break;
      }
      case 1: {
        SX1268 *p = new SX1268(_mod);
        codes[t] = p->begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                            RADIO_SYNC_WORD, RADIO_TX_POWER_DBM,
                            RADIO_PREAMBLE_LEN, 0);
        cand = p;
        break;
      }
      default: {
        LLCC68 *p = new LLCC68(_mod);
        codes[t] = p->begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                            RADIO_SYNC_WORD, RADIO_TX_POWER_DBM,
                            RADIO_PREAMBLE_LEN, 0);
        cand = p;
        break;
      }
    }
    if (codes[t] == RADIOLIB_ERR_NONE) {
      _r = cand;
      _chipClass = kNames[t];
      _beginCode = RADIOLIB_ERR_NONE;
      break;
    }
    delete cand;
    _beginCode = codes[t];
  }
  if (_r == nullptr) {
    _ready = false;
    return false;
  }

  // 版本字符串寄存器 0x0320（RadioLib findChip 已用它确认芯片在位；这里读出来显示）
  memset(_ver, 0, sizeof(_ver));
  _mod->SPIreadRegisterBurst(RADIOLIB_SX126X_REG_VERSION_STRING,
                             sizeof(_ver), _ver);
  memset(_chip, 0, sizeof(_chip));
  for (uint8_t i = 0; i < 6 && i < sizeof(_chip) - 1; i++) {
    const char c = (char)_ver[i];
    if (c < 0x20 || c > 0x7E) break;
    _chip[i] = c;
  }
  if (_chip[0] == 0) strcpy(_chip, "?");
  _airTheo = (uint32_t)_r->getTimeOnAir(10);

  _dlMtx = xSemaphoreCreateMutex();
  _ready = true;
#if PIN_SX_DIO1 >= 0
  // DIO1（G04）已接线：装中断，用于精确到达时刻。未接线时保持轮询时间戳。
  _self = this;
  pinMode(PIN_SX_DIO1, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_SX_DIO1), dio1Isr, RISING);
#endif
  _r->startReceive();
  return true;
}

void LoRaTest::start() {
  if (!_ready || _task) return;
  if (xTaskCreatePinnedToCore(taskEntry, "loratest", 4096, this, 5, &_task, 1) !=
      pdPASS) {
    _task = nullptr;
    note("射频任务创建失败");
  }
}

void LoRaTest::taskEntry(void *arg) { ((LoRaTest *)arg)->run(); }

// ---------------------------------------------------------------- 模式 / 频点
const char *LoRaTest::modeName(TestMode m) {
  switch (m) {
    case MODE_SCAN: return "SCAN(频点扫描)";
    case MODE_MON:  return "MON(被动监听)";
    case MODE_GW:   return "GW(TDMA网关)";
    case MODE_PING: return "PING(主动测试)";
    case MODE_ECHO: return "ECHO(应答器)";
    default: return "?";
  }
}

void LoRaTest::setMode(TestMode m) {
  if (m >= MODE_COUNT || m == _mode) return;
  _mode = m;
  _armed = false;
  if (m == MODE_PING) _pingBatch = PING_BATCH_DEFAULT;
  if (m != MODE_PING) _pingBatch = 0;
  note("模式 → %s（k=%u %.1fMHz）", modeName(m), (unsigned)_grid,
       (double)(TDMA_STD_BASE_MHZ + (float)_grid * TDMA_STD_STEP_MHZ));
}

void LoRaTest::setGrid(uint8_t k) {
  if (k >= TDMA_STD_CHANNELS) k = TDMA_STD_CHANNELS - 1;
  _grid = k;
  _gridReq++;      // 射频任务下次循环比对后应用（避免竞态）
  _armed = false;
}

void LoRaTest::rescan() { _rescan = true; }

void LoRaTest::sendPing(uint16_t count) {
  _pingBatch = (count == 0) ? 0xFFFF : count;
}

void LoRaTest::broadcastStart() { _bcStartReq = 1; }
void LoRaTest::broadcastEnd() { _bcStartReq = 2; }

void LoRaTest::applyGrid(uint8_t k, bool force) {
  (void)force;
  if (k >= TDMA_STD_CHANNELS) k = TDMA_STD_CHANNELS - 1;
  const float f = TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ;
  _r->standby();          // setFrequency 需 standby 态
  _r->setFrequency(f);
  _r->startReceive();
  _grid = k;
}

// ---------------------------------------------------------------- 收发
bool LoRaTest::pollRx(RxEvt &out) {
  if (!_r) return false;

  const uint16_t irq = _r->getIrqStatus();
  const uint16_t evIrq = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR |
                         RADIOLIB_SX126X_IRQ_HEADER_ERR;
  if (!(irq & evIrq)) return false;

  const bool crcErr =
      (irq & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR)) != 0;

  // RadioLib 文档：getPacketLength 必须在 readData 之前调用
  const size_t len = _r->getPacketLength();
  uint8_t buf[16];
  memset(buf, 0, sizeof(buf));
  const int16_t st = _r->readData(buf, sizeof(buf));   // 读数据 + 清 IRQ
  if (st != RADIOLIB_ERR_NONE && st != RADIOLIB_ERR_CRC_MISMATCH) {
    _r->startReceive();     // 异常：重武装接收
    return false;
  }
  const float rssiF = _r->getRSSI(true);   // 包 RSSI（非瞬时）
  const float snrF = _r->getSNR();
  const float feF = _r->getFrequencyError();
  // 到达时刻：优先用 DIO1 中断记录的时刻（±µs）；无中断时（例如 CRC 错的包不拉
  // DIO1）退回轮询时刻
  const bool hadIrq = _rxIrqLatched;
  const uint32_t irqUs = _rxIrqUs;
  _rxIrqLatched = false;
  const uint32_t us = hadIrq ? irqUs : micros();
  const uint32_t ms = millis();

  out.us = us;
  out.ms = ms;
  out.rssi = (int16_t)lroundf(rssiF);
  out.snrX4 = (int8_t)lroundf(snrF * 4.0f);
  out.freqErrHz = (int16_t)lroundf(feF);
  out.len = (uint16_t)len;
  out.crcOk = !crcErr;
  out.tdmaOk = false;
  out.grid = _grid;
  out.slotFrame = false;
  out.offUs = 0;
  out.respUs = 0;
  out.n = (uint8_t)((len < sizeof(out.raw)) ? len : sizeof(out.raw));
  memcpy(out.raw, buf, out.n);

  if (!crcErr && len == 10) {
    out.tdmaOk = tdmaDecode(buf, out.f);
    statsCountRx(out.tdmaOk ? 0 : 3);
  } else if (crcErr) {
    statsCountRx(1);
  } else {
    statsCountRx(2);   // 长度异常（同频的其它 LoRa 网络/干扰）
  }
  return true;
}

bool LoRaTest::pollRxUntil(RxEvt &out, uint32_t deadlineUs) {
  for (;;) {
    if (pollRx(out)) return true;
    if ((int32_t)(deadlineUs - micros()) <= 0) return false;
  }
}

bool LoRaTest::pollRxBusy(RxEvt &out, uint32_t busyUs) {
  return pollRxUntil(out, micros() + busyUs);
}

int16_t LoRaTest::txRaw(const uint8_t *buf, size_t len, uint32_t *startUs,
                        uint32_t *airUs) {
  if (!_r) return RADIOLIB_ERR_UNKNOWN;
  _busyTx = true;
  _txWindow = true;         // 屏蔽 TX_DONE 引起的 DIO1 跳变（别当成收包时刻）
  const uint32_t t0 = micros();
  int16_t st = _r->startTransmit((uint8_t *)buf, len);
  if (st != RADIOLIB_ERR_NONE) {
    _busyTx = false;
    _txWindow = false;
    statsOnTx(0, false);
    if (startUs) *startUs = t0;
    if (airUs) *airUs = 0;
    return st;
  }
  // DIO1 未接线 → 轮询 IRQ 寄存器等 TX_DONE（不能用阻塞 transmit）
  const uint32_t deadline = micros() + 150000;   // 150ms 兜底
  bool done = false;
  for (;;) {
    if (_r->getIrqStatus() & RADIOLIB_SX126X_IRQ_TX_DONE) { done = true; break; }
    if ((int32_t)(micros() - deadline) > 0) break;
  }
  const uint32_t air = (uint32_t)(micros() - t0);
  _r->finishTransmit();     // 清 IRQ + standby
  _txWindow = false;
  _busyTx = false;

  if (startUs) *startUs = t0;
  if (airUs) *airUs = air;
  statsOnTx(air, done);
  return done ? RADIOLIB_ERR_NONE : RADIOLIB_ERR_TX_TIMEOUT;
}

int16_t LoRaTest::txFrame(const TdmaFrame &f, uint32_t *startUs,
                          uint32_t *airUs) {
  uint8_t buf[10];
  tdmaEncode(f, buf);
  return txRaw(buf, 10, startUs, airUs);
}

// ---------------------------------------------------------------- 统计入口
void LoRaTest::onFrameStats(const RxEvt &e, bool slotFrame, int32_t offUs,
                            uint32_t respUs) {
  if (!e.tdmaOk) return;
  statsOnFrame(e.f, e.rssi, (float)e.snrX4 / 4.0f, e.freqErrHz, e.ms, slotFrame,
               offUs, respUs);
}

// ---------------------------------------------------------------- GW 设备表
int8_t LoRaTest::slotOf(uint8_t idx) const {
  for (uint8_t i = 0; i < _gwCount; i++) {
    if (_gw[i].valid && _gw[i].idx == idx) return (int8_t)_gw[i].slot;
  }
  return -1;
}

int8_t LoRaTest::gwFind(uint8_t idx) const {
  for (uint8_t i = 0; i < _gwCount; i++) {
    if (_gw[i].valid && _gw[i].idx == idx) return (int8_t)i;
  }
  return -1;
}

bool LoRaTest::dlPush(const TdmaFrame &f) {
  if (!_dlMtx) return false;
  bool ok = false;
  xSemaphoreTake(_dlMtx, portMAX_DELAY);
  const uint8_t next = (uint8_t)((_dlTail + 1) % (TDMA_MAX_SLOTS + 8));
  if (next != _dlHead) {
    _dl[_dlTail] = f;
    _dlTail = next;
    ok = true;
  }
  xSemaphoreGive(_dlMtx);
  return ok;
}

bool LoRaTest::dlPop(TdmaFrame &out) {
  if (!_dlMtx) return false;
  bool ok = false;
  xSemaphoreTake(_dlMtx, portMAX_DELAY);
  if (_dlHead != _dlTail) {
    out = _dl[_dlHead];
    _dlHead = (uint8_t)((_dlHead + 1) % (TDMA_MAX_SLOTS + 8));
    ok = true;
  }
  xSemaphoreGive(_dlMtx);
  return ok;
}

void LoRaTest::gwReSlot() {
  _mapVer++;
  _n = (_gwCount > 0) ? _gwCount : 1;
  if (_n > TDMA_MAX_SLOTS) _n = TDMA_MAX_SLOTS;
  for (uint8_t i = 0; i < _gwCount; i++) {
    _gw[i].slot = i;
    TdmaFrame a;
    tdmaMake(a, _gw[i].idx, TF_ASSIGN, 0, 0, NULL);
    a.payload[0] = _grid;          // 设备据此跳频（本板只用一个射频）
    a.payload[1] = _gw[i].slot;
    a.payload[2] = _n;
    a.payload[3] = _mapVer;
    dlPush(a);
    DevStat *d = statsDev(_gw[i].idx);
    if (d) {
      d->assigned = true;
      d->slot = _gw[i].slot;
    }
  }
  note("重排 mapVer=%u N=%u（%u 台）", (unsigned)_mapVer, (unsigned)_n,
       (unsigned)_gwCount);
}

void LoRaTest::gwExpire() {
  const uint32_t now = millis();
  bool changed = false;
  for (uint8_t i = 0; i < _gwCount; i++) {
    if (_gw[i].valid && (now - _gw[i].lastSeenMs) > TDMA_DEV_TIMEOUT_MS) {
      note("设备超时移除 dev=%u（%lums 无帧）", (unsigned)_gw[i].idx,
           (unsigned long)(now - _gw[i].lastSeenMs));
      _gw[i].valid = false;
      changed = true;
    }
  }
  if (!changed) return;
  uint8_t w = 0;
  for (uint8_t i = 0; i < _gwCount; i++) {
    if (_gw[i].valid) _gw[w++] = _gw[i];
  }
  _gwCount = w;
  gwReSlot();
}

void LoRaTest::gwOnUplink(const RxEvt &e, bool regSlot) {
  const TdmaFrame &f = e.f;
  if (f.devIdx == TF_BROADCAST_IDX) return;
  const uint32_t now = millis();

  const int8_t i = gwFind(f.devIdx);
  if (i >= 0) {
    _gw[i].lastSeenMs = now;
    // 注册时延 = ASSIGN 发完 → 本设备首个时隙帧到达
    DevStat *d = statsDev(f.devIdx);
    if (d && !regSlot && !d->firstSlotSeen) {
      d->firstSlotSeen = true;
      if (d->assignDoneUs != 0) {
        d->regLatUs = (uint32_t)(e.us - d->assignDoneUs);
        d->regLatSeen = true;
      }
    }
    return;
  }

  if (f.type != TF_JOIN) return;   // 未注册设备只接受 JOIN

  char id[8];
  memcpy(id, f.payload, 5);
  id[5] = 0;

  if (_gwCount >= TDMA_MAX_SLOTS) {
    TdmaFrame a;
    tdmaMake(a, f.devIdx, TF_ASSIGN, 0, 0, NULL);
    a.payload[0] = _grid;
    a.payload[1] = 0xFF;           // 信道满
    a.payload[2] = _n;
    a.payload[3] = _mapVer;
    dlPush(a);
    note("信道满（%u 台），拒绝 dev=%u", (unsigned)_gwCount, (unsigned)f.devIdx);
    return;
  }

  _gw[_gwCount].idx = f.devIdx;
  _gw[_gwCount].slot = _gwCount;
  _gw[_gwCount].lastSeenMs = now;
  _gw[_gwCount].valid = true;
  _gwCount++;
  note("JOIN dev=%u \"%s\"%s → N=%u", (unsigned)f.devIdx, id,
       (f.flags & TF_FLAG_HELMET) ? " [头盔]" : "", (unsigned)_gwCount);
  gwReSlot();
}

// ---------------------------------------------------------------- 广播 START/END
void LoRaTest::handleBcReq() {
  const uint32_t req = _bcStartReq;
  if (!req) return;
  _bcStartReq = 0;
  if (_mode == MODE_MON) {
    note("MON 为被动监听模式，不发射：t/e 命令已忽略");
    return;
  }
  TdmaFrame f;
  if (req == 1) {
    tdmaMake(f, TF_BROADCAST_IDX, TF_START, 0, 0, NULL);
    statsG().startTx++;
  } else {
    tdmaMake(f, TF_BROADCAST_IDX, TF_END, 0, 0, NULL);
    statsG().endTx++;
  }
  if (_mode == MODE_GW) {
    dlPush(f);                    // 走下行窗（TDMA 时隙内发射）
    return;
  }
  uint32_t s = 0, a = 0;
  txFrame(f, &s, &a);
  _r->startReceive();
  note("%s 已广播", req == 1 ? "START" : "END");
}

// ---------------------------------------------------------------- 任务主体
void LoRaTest::run() {
  for (;;) {
    if (_rescan) {
      _rescan = false;
      _mode = MODE_SCAN;
    }
    if (_gridApplied != _gridReq) {
      applyGrid(_grid, true);
      _gridApplied = _gridReq;
    }
    switch (_mode) {
      case MODE_SCAN: runScan(); break;
      case MODE_MON:  runMon();  break;
      case MODE_GW:   runGw();   break;
      case MODE_PING: runPing(); break;
      case MODE_ECHO: runEcho(); break;
      default: vTaskDelay(pdMS_TO_TICKS(50)); break;
    }
    vTaskDelay(1);
  }
}

// ---- SCAN：20 个标准栅格频点逐个测底噪 + 驻留收帧 ----
void LoRaTest::runScan() {
  _scanActive = true;
  int8_t bestK = -1;
  uint16_t bestFrames = 0;
  int8_t signalK = -1;   // 有能量但解不出帧（疑似同频干扰/非本协议）

  for (uint8_t k = 0; k < TDMA_STD_CHANNELS; k++) {
    if (_mode != MODE_SCAN || _rescan) break;
    _r->standby();
    _r->setFrequency(TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ);
    _grid = k;
    _gridApplied = _gridReq;
    _r->startReceive();
    delay(5);

    long sum = 0;
    int16_t mx = -200;
    for (uint16_t i = 0; i < SCAN_SAMPLES; i++) {
      const int16_t v = (int16_t)lroundf(_r->getRSSI(false));  // 瞬时 RSSI
      if (v > mx) mx = v;
      sum += v;
      delay(SCAN_INTERVAL_MS);
    }
    FreqQual &q = statsG().freq[k];
    q.avg = (float)sum / (float)SCAN_SAMPLES;
    q.mx = mx;
    q.frames = 0;

    const uint32_t t0 = millis();
    while ((millis() - t0) < SCAN_DWELL_MS && _mode == MODE_SCAN && !_rescan) {
      RxEvt e;
      if (pollRx(e)) {
        e.grid = k;
        q.frames++;
        onFrameStats(e, false, 0, 0);
        pushEvt(e);
      } else {
        vTaskDelay(1);
      }
    }
    _r->standby();

    if (q.frames > 0) {
      if (q.frames > bestFrames) {
        bestFrames = q.frames;
        bestK = (int8_t)k;
      }
    } else if (signalK < 0 &&
               (q.mx > SCAN_NOISY_MAX_DBM || q.avg > SCAN_NOISY_AVG_DBM)) {
      signalK = (int8_t)k;
    }
  }

  statsG().freqValid = true;
  _scanActive = false;
  _scanPassDone++;

  if (_mode != MODE_SCAN || _rescan) return;

#if SCAN_AUTO_MON
  if (bestK >= 0) {
    note("扫描完成：占用频点 k=%d（%.1fMHz）共 %u 帧 → 锁定并转 MON",
         (int)bestK, (double)(TDMA_STD_BASE_MHZ + (float)bestK * TDMA_STD_STEP_MHZ),
         (unsigned)bestFrames);
    _grid = (uint8_t)bestK;
    _gridReq++;
  } else if (signalK >= 0) {
    note("扫描完成：k=%d 有射频能量但无合法 TDMA 帧（疑似干扰/非同协议）→ 转 MON k=%d",
         (int)signalK, (int)signalK);
    _grid = (uint8_t)signalK;
    _gridReq++;
  } else {
    note("扫描完成：20 个频点均无 TDMA 帧（对端未上电/距离过远/天线未接）→ 转 MON k=%u",
         (unsigned)_grid);
  }
  _mode = MODE_MON;
#else
  while (_mode == MODE_SCAN && !_rescan) vTaskDelay(pdMS_TO_TICKS(SCAN_IDLE_MS));
#endif
}

// ---- MON：锁定单频点被动监听（绝不发射）----
void LoRaTest::runMon() {
  _r->startReceive();
  uint32_t lastNoise = 0;
  while (_mode == MODE_MON && !_rescan) {
    if (_gridApplied != _gridReq) break;   // 换频点：回 run() 重新应用
    RxEvt e;
    if (pollRx(e)) {
      onFrameStats(e, false, 0, 0);
      pushEvt(e);
    } else {
      vTaskDelay(1);
    }
    if ((millis() - lastNoise) >= 500) {
      lastNoise = millis();
      _idleRssi = _r->getRSSI(false);
    }
    handleBcReq();     // MON：只会打印"已忽略"提示
  }
}

// ---- GW：TDMA 网关（结构与 TdmaMac::runGateway 相同，另有两处必要修正）----
// 修正 1（相位）：时隙/注册窗以「实际信标空口起点 bcStart」为基准，而不是本地
//   预定时刻 sfStart —— 设备端 lockFromBeacon 用的正是信标空口起点，这样两端相位一致。
// 修正 2（周期）：超帧周期固定为 30+10N+GW_REG_RX_EXTRA_US+1ms，不随本超帧实际耗时
//   变化。若直接沿用 TdmaMac 的「sfStart + 30+10N」而注册窗又超时 2ms，误差会逐帧
//   累积 → 设备端每个超帧都晚到 → 最终错过信标窗口（±1.5ms/+13ms）而失锁。
//   本固件每超帧还固定 vTaskDelay(1)：全部窗口都是忙等，必须让出 CPU 喂 IDLE 看门狗。
void LoRaTest::runGw() {
  if (!_armed) {
    _sfStartUs = micros() + 20000;   // 首个超帧栅格
    _armed = true;
    _lastBcStart = 0;
  }
  _r->standby();

  while (_mode == MODE_GW && !_rescan) {
    if (_gridApplied != _gridReq) break;
    handleBcReq();

    const uint32_t sfStart = _sfStartUs;
    const int32_t toGo = (int32_t)(sfStart - micros());
    if (toGo > 3000) vTaskDelay(pdMS_TO_TICKS((toGo - 2000) / 1000));
    while ((int32_t)(sfStart - micros()) > 0) { }   // 末段忙等对齐

    // 1) 信标窗：TF_BEACON（超帧计数 + mapVer/N）
    TdmaFrame b;
    tdmaMake(b, TF_BROADCAST_IDX, TF_BEACON, 0, 0, NULL);
    const uint32_t c = _sfCounter++;
    b.payload[0] = (uint8_t)(c >> 24);
    b.payload[1] = (uint8_t)(c >> 16);
    b.payload[2] = (uint8_t)(c >> 8);
    b.payload[3] = (uint8_t)c;
    b.payload[4] = (uint8_t)((_mapVer << 5) | (_n & 0x1F));
    uint32_t bcStart = sfStart, bcAir = 0;
    const int16_t bcSt = txFrame(b, &bcStart, &bcAir);
    statsG().beaconTx++;
    if (bcSt != RADIOLIB_ERR_NONE) {
      note("信标发射异常：%d（空口 %luus）", (int)bcSt, (unsigned long)bcAir);
    }
    const uint32_t phase = bcStart;    // 本超帧相位基准（设备端同一点）

    // 实测信标周期 → 暴露超帧是否跑偏
    if (_lastBcStart != 0) {
      const uint32_t period = (uint32_t)(phase - _lastBcStart);
      if (period < statsG().sfMin) statsG().sfMin = period;
      if (period > statsG().sfMax) statsG().sfMax = period;
      statsG().sfSum += period;
      statsG().sfN++;
    }
    _lastBcStart = phase;

    // 2) 广播下行窗：本超帧发 1 帧（与 TdmaMac 一致）
    TdmaFrame dl;
    if (dlPop(dl)) {
      uint32_t s2 = 0, a2 = 0;
      txFrame(dl, &s2, &a2);
      statsG().dlTx++;
      if (dl.type == TF_ASSIGN) {
        statsG().assignTx++;
        DevStat *d = statsDev(dl.devIdx);
        if (d) {
          d->assigned = true;
          d->slot = dl.payload[1];
          d->assignDoneUs = micros();   // 注册时延基准
        }
      }
    }

    // 3) 设备时隙共听（武装一次后连续 RX；槽间不重武装，靠 IRQ 寄存器取包）
    _r->startReceive();
    for (uint8_t k = 0; k < _n && _mode == MODE_GW && !_rescan; k++) {
      const uint32_t sStart =
          phase + TDMA_BEACON_US + TDMA_DL_US + (uint32_t)k * TDMA_SLOT_US;
      RxEvt e;
      if (pollRxUntil(e, sStart + TDMA_SLOT_US + GW_SLOT_RX_EXTRA_US)) {
        e.slotFrame = true;
        e.offUs = (int32_t)((e.us - TDMA_AIR_US) - sStart);
        e.respUs = (uint32_t)((e.us - TDMA_AIR_US) - phase);
        gwOnUplink(e, false);
        onFrameStats(e, true, e.offUs, e.respUs);
        pushEvt(e);
      }
    }

    // 4) 注册时隙（新设备 JOIN / 失配设备补 JOIN）
    const uint32_t regStart =
        phase + TDMA_BEACON_US + TDMA_DL_US + (uint32_t)_n * TDMA_SLOT_US;
    if (_mode == MODE_GW && !_rescan) {
      RxEvt e;
      if (pollRxUntil(e, regStart + TDMA_REG_US + GW_REG_RX_EXTRA_US)) {
        e.slotFrame = false;   // 注册槽有 0-6ms 随机退避，不作为时隙偏移样本
        e.offUs = 0;
        e.respUs = (uint32_t)((e.us - TDMA_AIR_US) - phase);
        gwOnUplink(e, true);
        onFrameStats(e, false, 0, 0);
        pushEvt(e);
      }
    }

    _r->standby();
    gwExpire();

    // 下一个超帧：30+10N + 注册窗裕量 + 1ms（恒定周期；_n 可能已因本次 JOIN/超时改变）
    const uint32_t P = TDMA_BEACON_US + TDMA_DL_US +
                       (uint32_t)_n * TDMA_SLOT_US + TDMA_REG_US;
    vTaskDelay(1);   // 让出 CPU（喂 IDLE 看门狗 + 打印任务）
    _sfStartUs = phase + P + GW_REG_RX_EXTRA_US;
  }
  _armed = false;
}

// ---- PING：主动 ping（0x1E）等 ECHO 应答（0x1F）----
void LoRaTest::pingOnRx(const RxEvt &e) {
  if (e.tdmaOk && e.f.type == TF_TEST_PONG) {
    statsOnPong(e.f.seq, e.us);
  }
  onFrameStats(e, false, 0, 0);
  pushEvt(e);
}

void LoRaTest::runPing() {
  _r->startReceive();
  while (_mode == MODE_PING && !_rescan) {
    if (_gridApplied != _gridReq) break;
    handleBcReq();

    if (_pingBatch == 0) {          // 待命：只收，等串口 p 命令
      RxEvt e;
      if (pollRx(e)) {
        pingOnRx(e);
      } else {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      statsPingSweep(micros());
      continue;
    }
    if (_pingBatch != 0xFFFF) _pingBatch--;

    _pingSeq++;
    if (_pingSeq == 0) _pingSeq = 1;
    const uint16_t ps = _pingSeq;
    TdmaFrame p;
    uint8_t pl[5] = { 'L', 'T', 'P', 'G', 0 };
    tdmaMake(p, PING_TARGET_IDX, TF_TEST_PING, 0, ps, pl);
    uint32_t s0 = 0, a0 = 0;
    txFrame(p, &s0, &a0);
    statsOnPingSent(ps, s0);   // 以实际空口起点为 RTT 基准
    _r->startReceive();

    // 忙等高精度窗口（RTT 精度靠这里）
    const uint32_t busyEnd = micros() + PING_BUSY_US;
    while ((int32_t)(busyEnd - micros()) > 0 && _mode == MODE_PING && !_rescan) {
      RxEvt e;
      if (pollRx(e)) pingOnRx(e);
    }

    // 剩余周期慢轮询（顺带收下别的帧）
    const uint32_t t2 = millis() + PING_PERIOD_MS;
    while ((int32_t)(t2 - millis()) > 0 && _mode == MODE_PING && !_rescan) {
      RxEvt e;
      if (pollRx(e)) {
        pingOnRx(e);
      } else {
        vTaskDelay(1);
      }
      statsPingSweep(micros());
    }
  }
}

// ---- ECHO：应答器（立即回 PONG）----
void LoRaTest::echoOnRx(const RxEvt &e) {
  if (e.tdmaOk && e.f.type == TF_TEST_PING) {
#if PING_ECHO_DELAY_MS > 0
    delay(PING_ECHO_DELAY_MS);
#endif
    TdmaFrame p;
    tdmaMake(p, ECHO_DEV_IDX, TF_TEST_PONG, 0, e.f.seq, NULL);
    uint32_t s = 0, a = 0;
    txFrame(p, &s, &a);
    _r->startReceive();
  }
  onFrameStats(e, false, 0, 0);
  pushEvt(e);
}

void LoRaTest::runEcho() {
  _r->startReceive();
  while (_mode == MODE_ECHO && !_rescan) {
    if (_gridApplied != _gridReq) break;
    handleBcReq();
    RxEvt e;
    if (pollRx(e)) {
      echoOnRx(e);
    } else {
      vTaskDelay(1);
    }
  }
}
