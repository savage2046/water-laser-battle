#include "LoRaDev.h"
#include <math.h>
#include <string.h>
#include <esp_system.h>   // esp_random()

// ============================================================================
// LoRa 网关自动连接 + 1s 心跳 实现
// ============================================================================

LoRaDev g_dev;
LoRaDev *LoRaDev::_self = nullptr;

// ---------------------------------------------------------------- DIO1 中断
// G04 = SX1268 DIO1：startReceive() 把 DIO1 掩码设成 RX_DONE（+超时），
// 上升沿即"收到一个包"。在 ISR 里记 micros() → ±µs 到达时刻。
void IRAM_ATTR LoRaDev::dio1Isr() {
  LoRaDev *s = _self;
  if (s == nullptr || s->_txWindow) return;
  s->_irqCount++;
  s->_rxIrqUs = micros();
  s->_rxIrqLatched = true;
}

// ---------------------------------------------------------------- 初始化
bool LoRaDev::begin() {
  SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);

  _mod = new Module(PIN_SX_NSS, PIN_SX_DIO1, PIN_SX_RST, PIN_SX_BUSY);

  // ---- 自动探测芯片类型（关键！）----
  // RadioLib 用**版本字符串**校验身份：SX1262 类只认 "SX1261"、SX1268 类只认
  // "SX1268"、LLCC68 类只认 "LLCC68"。拿错类会重试 10 次后返回
  // -2 CHIP_NOT_FOUND，**看起来像硬件故障，其实焊接全好**。
  // U2 = Ra-01S（SX1268）→ 早期固件用 SX1262 类，所以 LoRa 从未真正跑起来。
  static const char *kNames[3] = { "SX1262(认 SX1261)", "SX1268", "LLCC68" };
  _st.beginCode = RADIOLIB_ERR_CHIP_NOT_FOUND;
  for (uint8_t t = 0; t < 3; t++) {
    // LoRa 的 begin(freq,...) 只在派生类上声明（基类是 GFSK 版）→ 用具体类型调
    SX126x *cand = nullptr;
    int16_t st = RADIOLIB_ERR_CHIP_NOT_FOUND;
    switch (t) {
      case 0: {
        SX1262 *p = new SX1262(_mod);
        st = p->begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                      RADIO_SYNC_WORD, RADIO_TX_POWER_DBM, RADIO_PREAMBLE_LEN, 0);
        cand = p;
        break;
      }
      case 1: {
        SX1268 *p = new SX1268(_mod);
        st = p->begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                      RADIO_SYNC_WORD, RADIO_TX_POWER_DBM, RADIO_PREAMBLE_LEN, 0);
        cand = p;
        break;
      }
      default: {
        LLCC68 *p = new LLCC68(_mod);
        st = p->begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                      RADIO_SYNC_WORD, RADIO_TX_POWER_DBM, RADIO_PREAMBLE_LEN, 0);
        cand = p;
        break;
      }
    }
    if (st == RADIOLIB_ERR_NONE) {
      _r = cand;
      _chipClass = kNames[t];
      _st.beginCode = RADIOLIB_ERR_NONE;
      break;
    }
    delete cand;
    _st.beginCode = st;
  }
  if (_r == nullptr) {
    _ready = false;
    _st.ready = false;
    return false;
  }
  dsLog("[T4] RadioLib 类 = %s（自动探测命中）\n", _chipClass);
  _mod->SPIreadRegisterBurst(RADIOLIB_SX126X_REG_VERSION_STRING, sizeof(_ver), _ver);
  _airTheo = (uint32_t)_r->getTimeOnAir(10);
#if PIN_SX_DIO1 >= 0
  // DIO1（G04）已接线：装中断用于精确到达时刻；未接线则退回轮询时间戳
  _self = this;
  pinMode(PIN_SX_DIO1, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_SX_DIO1), dio1Isr, RISING);
#endif
  _r->startReceive();
  _ready = true;
  _st.ready = true;
  return true;
}

void LoRaDev::start() {
  if (!_ready || _task) return;
  if (xTaskCreatePinnedToCore(taskEntry, "loradev", 4096, this, 5, &_task, 1) !=
      pdPASS) {
    _task = nullptr;
    dsLog("[T4] 设备任务创建失败\n");
  }
}

void LoRaDev::taskEntry(void *arg) { ((LoRaDev *)arg)->run(); }

void LoRaDev::requestRescan() { _needRescan = true; }
void LoRaDev::setGridManual(uint8_t k) {
  _manualK = k;
  _manualGrid = true;
  _needRescan = true;
}

void LoRaDev::applyGrid(uint8_t k) {
  _r->standby();
  _r->setFrequency(TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ);
  _r->startReceive();
  _st.grid = k;
}

// ---------------------------------------------------------------- 收发
bool LoRaDev::pollRx(LoRaPkt &out) {
  if (!_r) return false;
  const uint16_t irq = _r->getIrqStatus();
  const uint16_t evIrq = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR |
                         RADIOLIB_SX126X_IRQ_HEADER_ERR;
  if (!(irq & evIrq)) return false;
  const bool crcErr =
      (irq & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR)) != 0;

  const size_t len = _r->getPacketLength();    // 必须在 readData 之前
  uint8_t buf[16];
  memset(buf, 0, sizeof(buf));
  const int16_t st = _r->readData(buf, sizeof(buf));
  if (st != RADIOLIB_ERR_NONE && st != RADIOLIB_ERR_CRC_MISMATCH) {
    _r->startReceive();
    return false;
  }
  const uint32_t us = micros();
  out.ms = millis();
  out.rssi = (int16_t)lroundf(_r->getRSSI(true));
  out.snrX4 = (int8_t)lroundf(_r->getSNR() * 4.0f);
  out.feHz = (int16_t)lroundf(_r->getFrequencyError());
  // 到达时刻：优先用 DIO1 中断记录的时刻（±µs）；无中断（如 CRC 错的包不拉 DIO1）
  // 时退回轮询时刻
  const bool hadIrq = _rxIrqLatched;
  const uint32_t irqUs = _rxIrqUs;
  _rxIrqLatched = false;
  out.startUs = (hadIrq ? irqUs : us) - TDMA_AIR_US;   // 估算空口起点
  out.tdmaOk = (!crcErr && len == 10 && tdmaDecode(buf, out.f));
  return true;
}

int16_t LoRaDev::txFrame(uint8_t type, const uint8_t *payload5, uint8_t flags,
                         uint32_t *startUs, uint32_t *airUs) {
  if (!_ready) return RADIOLIB_ERR_UNKNOWN;
  uint8_t buf[10];
  TdmaFrame f;
  _seq++;
  tdmaMake(f, DEV_IDX, type, flags, _seq, payload5);
  tdmaEncode(f, buf);

  _r->standby();                              // 配置命令需 standby
  _txWindow = true;                           // 屏蔽 TX_DONE 的 DIO1 跳变
  const uint32_t t0 = micros();
  int16_t st = _r->startTransmit(buf, 10);
  if (st != RADIOLIB_ERR_NONE) {
    _txWindow = false;
    _st.txFail++;
    if (startUs) *startUs = t0;
    if (airUs) *airUs = 0;
    return st;
  }
  const uint32_t deadline = micros() + 150000;
  bool done = false;
  for (;;) {
    if (_r->getIrqStatus() & RADIOLIB_SX126X_IRQ_TX_DONE) { done = true; break; }
    if ((int32_t)(micros() - deadline) > 0) break;
  }
  const uint32_t air = (uint32_t)(micros() - t0);
  _r->finishTransmit();
  _txWindow = false;
  if (startUs) *startUs = t0;
  if (airUs) *airUs = air;
  _st.airUsLast = air;
  if (!done) _st.txFail++;
  return done ? RADIOLIB_ERR_NONE : RADIOLIB_ERR_TX_TIMEOUT;
}

// ---------------------------------------------------------------- 扫网关
// 20 个标准频点，每点听 GW_SCAN_DWELL_MS；命中即锁相并返回 true。
// 优先试上次成功的频点（重连快）。
bool LoRaDev::scanGateway() {
  _st.locked = false;
  _st.assigned = false;
  _st.scans++;

  // 手动锁定频点：只试指定频点（调试用）
  if (_manualGrid) {
    if (tryGrid(_manualK)) return true;
    dsLog("[T4] 手动频点 k=%u (%.1fMHz) 未听到信标（%dms）\n", (unsigned)_manualK,
          (double)(TDMA_STD_BASE_MHZ + (float)_manualK * TDMA_STD_STEP_MHZ),
          GW_SCAN_DWELL_MS);
    return false;
  }

  // 先试上次成功的频点（重连只需一个驻留窗），失败再全栅格扫
  if (_lastGoodGrid >= 0 && tryGrid((uint8_t)_lastGoodGrid)) return true;
  for (uint8_t k = 0; k < TDMA_STD_CHANNELS; k++) {
    if ((int8_t)k == _lastGoodGrid) continue;
    if (tryGrid(k)) return true;
  }
  dsLog("[T4] 未扫到网关信标（%d 个频点各 %dms）—— 网关是否在发信标？\n",
        TDMA_STD_CHANNELS, GW_SCAN_DWELL_MS);
  return false;
}

// 在频点 k 上驻留 GW_SCAN_DWELL_MS 听信标；听到即锁相并返回 true
bool LoRaDev::tryGrid(uint8_t k) {
  if (_needRescan) { _needRescan = false; return false; }
  applyGrid(k);
  const uint32_t t0 = millis();
  while ((millis() - t0) < GW_SCAN_DWELL_MS) {
    LoRaPkt p;
    if (pollRx(p)) {
      if (p.tdmaOk && p.f.type == TF_BEACON) {
        _lastGoodGrid = (int8_t)k;
        onRx(p);                              // 内部完成锁相
        dsLog("[T4] 听到网关信标：k=%u (%.1fMHz) N=%u rssi=%d snr=%.2f\n",
              (unsigned)k,
              (double)(TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ),
              (unsigned)_n, (int)p.rssi, (double)p.snrX4 / 4.0);
        return true;
      }
    } else {
      vTaskDelay(1);
    }
  }
  return false;
}

// ---------------------------------------------------------------- 收帧处理
void LoRaDev::onRx(const LoRaPkt &p) {
  if (!p.tdmaOk) { _st.rxBad++; return; }
  const TdmaFrame &f = p.f;

  if (f.type == TF_BEACON) {
    _st.beacons++;
    _st.lastBeaconMs = millis();
    _st.beaconRssi = p.rssi;
    _st.beaconSnrX4 = p.snrX4;
    _st.beaconFeHz = p.feHz;
    // 锁相：与网关同用"信标空口起点"作为超帧基准
    _sfPhaseUs = p.startUs;
    _sfCounter = ((uint32_t)f.payload[0] << 24) | ((uint32_t)f.payload[1] << 16) |
                 ((uint32_t)f.payload[2] << 8) | (uint32_t)f.payload[3];
    _n = f.payload[4] & 0x1F;
    if (_n < 1) _n = 1;
    if (_n > TDMA_MAX_SLOTS) _n = TDMA_MAX_SLOTS;
    _st.beaconMapVer = (uint8_t)(f.payload[4] >> 5);
    _st.n = _n;
    _st.locked = true;
    // 新超帧：允许再发一次时隙帧 / 注册帧
    _sfSlotSent = false;
    _sfRegSent = false;
#if JOIN_EVERY_SF > 1
    _sfJoinDue = ((_sfJoinSeq++ % JOIN_EVERY_SF) == 0);
#else
    _sfJoinDue = true;
#endif
    _joinJitterUs = (uint32_t)(esp_random() % JOIN_JITTER_US);
    // 网关重排（mapVer 变了）→ 回到注册流程补 JOIN
    if (_st.assigned && _st.beaconMapVer != _st.mapVer) {
      _st.assigned = false;
    }
    return;
  }

  // 下行寻址：广播或本机（与正式 TdmaMac 语义一致）
  if (f.devIdx != TF_BROADCAST_IDX && f.devIdx != DEV_IDX) return;

  if (f.type == TF_ASSIGN) {
    const uint8_t slot = f.payload[1];
    if (slot == 0xFF) {
      dsLog("[T4] 信道 k=%u 已满（网关拒绝），重新扫频\n", (unsigned)_st.grid);
      _needRescan = true;
      _st.locked = false;
      _st.assigned = false;
      return;
    }
    _st.assigned = true;
    _st.dlySlot = slot;
    _st.slot = slot;
    _st.dlyN = f.payload[2];
    _st.dlyMapVer = f.payload[3];
    _st.mapVer = f.payload[3];
    _st.assigns++;
    if (_st.connectMs == 0) _st.connectMs = millis();
    if (_joinSentMs) {
      _st.regLatMs = millis() - _joinSentMs;
      _joinSentMs = 0;
    }
    dsLog("[T4] 获得分配：k=%u slot=%u N=%u mapVer=%u（注册时延 %lums）\n",
          (unsigned)_st.grid, (unsigned)slot, (unsigned)f.payload[2],
          (unsigned)f.payload[3], (unsigned long)_st.regLatMs);
    return;
  }
  _st.dlRx++;
}

// ---------------------------------------------------------------- 发送
void LoRaDev::txHeartbeat(uint32_t slotAtUs) {
  const int32_t off = (int32_t)(micros() - slotAtUs);   // 实际起发相对期望时隙起点
  uint32_t s = 0, a = 0;
  const int16_t st = txFrame(TF_HB, NULL, 0, &s, &a);
  if (st == RADIOLIB_ERR_NONE) {
    if (_hbPending) _hbPending--;
    _st.hbTx++;
    const uint32_t now = millis();
    if (_st.lastHbMs != 0) {
      const uint32_t dt = now - _st.lastHbMs;
      if (_st.hbIntN == 0) { _st.hbIntMin = _st.hbIntMax = dt; }
      else {
        if (dt < _st.hbIntMin) _st.hbIntMin = dt;
        if (dt > _st.hbIntMax) _st.hbIntMax = dt;
      }
      _st.hbIntSum += dt;
      if (_st.hbIntN < 0xFFFF) _st.hbIntN++;
    }
    _st.lastHbMs = now;
    if (off > HB_LATE_WARN_US) _st.hbLate++;
    if (_st.hbOffN == 0) { _st.hbOffMin = _st.hbOffMax = off; }
    else {
      if (off < _st.hbOffMin) _st.hbOffMin = off;
      if (off > _st.hbOffMax) _st.hbOffMax = off;
    }
    _st.hbOffSum += off;
    if (_st.hbOffN < 0xFFFF) _st.hbOffN++;
  }
  _r->startReceive();
}

void LoRaDev::txJoin(uint32_t atUs) {
  uint8_t pl[5] = { 0 };
  const char *id = DEVICE_ID;
  for (uint8_t i = 0; i < 5 && id[i]; i++) pl[i] = (uint8_t)id[i];
  uint32_t s = 0, a = 0;
  const int16_t st = txFrame(TF_JOIN, pl, 0, &s, &a);
  if (st == RADIOLIB_ERR_NONE) {
    _st.joins++;
    _joinSentMs = millis();
    dsLog("[T4] 注册槽发 JOIN（k=%u slot偏移 %ldus）\n", (unsigned)_st.grid,
          (long)((int32_t)(s - atUs)));
  }
  _r->startReceive();
}

// ---------------------------------------------------------------- 主循环
void LoRaDev::run() {
  for (;;) {
    if (!_ready) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    if (_needRescan) { _needRescan = false; }
    if (!_st.locked) {
      if (!scanGateway()) continue;
    }
    runLocked();
    vTaskDelay(1);
  }
}

void LoRaDev::runLocked() {
  _r->startReceive();     // 连续 RX（抗抢占；不在窗口间 standby）
  _lastHbEnqMs = millis();

  while (_st.locked) {
    // 1) 收（信标/ASSIGN/下行）
    LoRaPkt p;
    if (pollRx(p)) {
      onRx(p);
      continue;
    }
    if (_needRescan) { _needRescan = false; _st.locked = false; break; }

    const uint32_t nowUs = micros();
    const uint32_t slotAt =
        _sfPhaseUs + TDMA_BEACON_US + TDMA_DL_US + (uint32_t)_st.slot * TDMA_SLOT_US;
    const uint32_t regAt =
        _sfPhaseUs + TDMA_BEACON_US + TDMA_DL_US + (uint32_t)_n * TDMA_SLOT_US;

    // 2) 每 1s 入队一个心跳
    if ((uint32_t)(millis() - _lastHbEnqMs) >= HB_PERIOD_MS) {
      _lastHbEnqMs = millis();
      if (_hbPending < HB_QUEUE_MAX) _hbPending++;
      _st.hbQueued++;
    }

    // 3) 发送：已分配 → 自身时隙发心跳；未分配 → 注册槽补 JOIN
    if (_st.assigned) {
      if (_forceJoin) {
        _forceJoin = false;
        _st.assigned = false;
      } else if (_hbPending > 0 && !_sfSlotSent) {
        const int32_t dt = (int32_t)(slotAt - nowUs);
        if (dt > 2000) {
          const uint32_t d = (uint32_t)(dt - 1500);
          vTaskDelay(pdMS_TO_TICKS(d / 1000));
          continue;
        }
        while ((int32_t)(slotAt - micros()) > 0) { }   // 末段忙等对齐（≤2ms）
        txHeartbeat(slotAt);
        _sfSlotSent = true;
        continue;
      }
    } else {
      if (_forceJoin) _forceJoin = false;
      if (!_sfRegSent && _sfJoinDue) {
        const uint32_t at = regAt + _joinJitterUs;
        const int32_t dt = (int32_t)(at - nowUs);
        if (dt > 2000) {
          const uint32_t d = (uint32_t)(dt - 1500);
          vTaskDelay(pdMS_TO_TICKS(d / 1000));
          continue;
        }
        while ((int32_t)(at - micros()) > 0) { }
        txJoin(at);
        _sfRegSent = true;
        continue;
      }
    }

    // 4) 掉线判定
    if (_st.lastBeaconMs != 0 &&
        (uint32_t)(millis() - _st.lastBeaconMs) > GW_BEACON_LOST_MS) {
      dsLog("[T4] %dms 未听到信标 → 判定掉线，重新扫频\n", GW_BEACON_LOST_MS);
      _st.locked = false;
      _st.assigned = false;
      _st.slot = 0xFF;
      break;
    }

    // 5) 兜底：万一某个信标被漏收，按标称周期推进本地相位（避免心跳被卡住）
    //    （收到信标时一律以信标为准；这里加 GW_SF_EXTRA_US 是为了不早于网关实际周期）
    {
      const uint32_t P = TDMA_BEACON_US + TDMA_DL_US +
                         (uint32_t)_n * TDMA_SLOT_US + TDMA_REG_US + GW_SF_EXTRA_US;
      if ((int32_t)(micros() - (_sfPhaseUs + P)) >= 0) {
        _sfPhaseUs += P;
        _sfSlotSent = false;
        _sfRegSent = false;
      }
    }
    vTaskDelay(1);
  }
}
