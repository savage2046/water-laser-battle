#include "TdmaMac.h"
#include <esp_system.h>  // esp_random()

// ===== 时隙栅格（与 config.h TDMA_* 及 docs/tdma-mac.md 保持一致）=====
// 10B 帧 @ SF7/BW500k/前导4 的空口时长 ≈ 9.28ms（前导 2.11 + 载荷 7.17）
static const uint32_t T_BE   = 10000;  // 信标窗
static const uint32_t T_DL   = 10000;  // 广播下行窗
static const uint32_t T_SLOT = 10000;  // 设备时隙
static const uint32_t T_REG  = 10000;  // 注册时隙
static const uint32_t T_AIR  = 9300;   // 10B 帧空口时长估算（前导4/SF7/500k）
static const uint32_t DEV_TIMEOUT_MS = 30000;  // 设备心跳超时（与 config.h 一致）

// ===== 接收窗取包裕量（2026-09-10 修正，见 docs/lora-gateway-test.md §4.2）=====
// 原实现统一用 +2000µs，但设备 tryJoin 带 0~6ms 随机退避：JOIN 最早 regStart
// 起发、最晚 regStart+6ms 起发，空口 9.3ms → 最晚 regStart+15.3ms 才收完。
// 裕量只给 2ms → **抖动 >2.7ms 的 JOIN 全部丢失（约 55%）**，注册被拖到秒级。
// 时隙窗 1.5ms 就够（设备用 waitUntil 精确起发，帧在时隙起点+9.3ms 结束）。
// 注册窗取 6ms：完整覆盖设备 tryJoin 的 0~6ms 退避（JOIN 最晚 regStart+6ms 起发
// → +15.3ms 收完，留 0.7ms 余量），且**不必改设备的退避幅度**、不损失抗碰撞性。
static const uint32_t T_SLOT_RX_MARGIN = 1500;
static const uint32_t T_REG_RX_MARGIN  = 6000;

// ===== 超帧尾部余量（2026-09-10 修正，见 docs/lora-gateway-test.md §4.3）=====
// 原实现按标称 "sfStart + 30+10N" 递推下一个超帧起点，而注册窗实际会超时
// 2~5ms → 每个超帧起点都比标称晚 → **误差逐帧累积**，最终网关信标晚到超过
// 设备信标窗（+13ms）→ 设备丢信标、每隔一个超帧才同步（表现成 50% 丢包）。
// 改为：以**实际信标空口起点**为基准、周期恒定 = 30+10N + T_SF_TAIL。
static const uint32_t T_SF_TAIL = T_REG_RX_MARGIN + 2000;   // = 7000µs

// MAC 任务栈（字）。单射频 4096 足够；多射频（T3 12 射频）须实测降至 2048-3072
// 或任务合并（见 docs/gateway-capacity.md §4.2/§6）——创建失败会跳过该射频。
#ifndef TDMA_TASK_STACK_WORDS
#define TDMA_TASK_STACK_WORDS 4096
#endif

TdmaMac tdma;

// ===== 环形队列（cap 必须为 2 的幂）=====
static bool ringPush(TdmaFrame *q, uint8_t cap, volatile uint8_t &head,
                     volatile uint8_t &tail, const TdmaFrame &f) {
  uint8_t next = (tail + 1) & (cap - 1);
  if (next == head) return false;  // 满
  q[tail] = f;
  tail = next;
  return true;
}

static bool ringPop(TdmaFrame *q, uint8_t cap, volatile uint8_t &head,
                    volatile uint8_t &tail, TdmaFrame &out) {
  if (head == tail) return false;  // 空
  out = q[head];
  head = (head + 1) & (cap - 1);
  return true;
}

// ===== 公共 =====
void TdmaMac::begin(Role role, uint8_t devIdx, SX126x *r,
                    const float *channels, uint8_t channelCount,
                    uint8_t myChannel, uint8_t maxSlots) {
  _role = role;
  _devIdx = devIdx;
  _r = r;
  _channels = channels;
  _channelCount = channelCount;
  _channel = myChannel;
  _maxSlots = maxSlots;
  _mutex = xSemaphoreCreateMutex();
  _txHead = _txTail = 0;
  _dlHead = _dlTail = 0;
  _rxHead = _rxTail = 0;
  _locked = false;
  _assigned = false;
  _mapVer = 0;
  _n = 1;
  _gwCount = 0;
  _sfCounter = 0;
  _task = nullptr;
  if (xTaskCreatePinnedToCore(taskEntry, "tdma", TDMA_TASK_STACK_WORDS, this, 5,
                              &_task, 1) != pdPASS) {
    Serial.printf("[tdma] FATAL: task create failed (stack=%u words)\n",
                  (unsigned)TDMA_TASK_STACK_WORDS);
    _task = nullptr;
  }
}

bool TdmaMac::send(uint8_t type, const uint8_t *payload5, uint8_t flags,
                   uint16_t seq) {
  TdmaFrame f;
  tdmaMake(f, _devIdx, type, flags, seq, payload5);
  bool ok;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  ok = ringPush(_tx, 8, _txHead, _txTail, f);
  xSemaphoreGive(_mutex);
  return ok;
}

bool TdmaMac::pollDownlink(TdmaFrame &out) {
  bool ok;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  ok = ringPop(_rx, 16, _rxHead, _rxTail, out);
  xSemaphoreGive(_mutex);
  return ok;
}

bool TdmaMac::sendDownlink(uint8_t type, const uint8_t *payload5,
                           uint8_t flags, uint8_t target, uint16_t seq) {
  TdmaFrame f;
  tdmaMake(f, target, type, flags, seq, payload5);
  bool ok;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  ok = ringPush(_dl, 32, _dlHead, _dlTail, f);
  xSemaphoreGive(_mutex);
  return ok;
}

bool TdmaMac::pollUplink(TdmaFrame &out) {
  bool ok;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  ok = ringPop(_rx, 16, _rxHead, _rxTail, out);
  xSemaphoreGive(_mutex);
  return ok;
}

// ===== 内部 =====
void TdmaMac::taskEntry(void *arg) {
  TdmaMac *m = (TdmaMac *)arg;
  for (;;) {
    if (m->_role == ROLE_GATEWAY) {
      m->runGateway();
    } else {
      m->runDevice();
    }
    vTaskDelay(1);
  }
}

void TdmaMac::waitUntil(int32_t targetUs) {
  while ((int32_t)(targetUs - micros()) > 0) {}
}

// 1ms 间隔轮询接收直到 deadline（持续 RX 由硬件保持，不丢包；轮询只负责取出）
// 2026-09-10 修正（见 docs/lora-gateway-test.md §4.6）：
//   ① 先查 IRQ 寄存器再 readData —— RadioLib 6.x 在"无 IRQ 事件"时也会照读缓冲并
//      返回 RADIOLIB_ERR_NONE，原写法会把上一包重复吐出来；
//   ② getPacketLength() 必须在 readData() **之前**取（RadioLib 文档要求）；
//   ③ 显式处理 CRC/报头错（硬件 CRC 失败时数据其实已读进缓冲，直接丢弃并重武装）。
bool TdmaMac::readPacketPoll(TdmaFrame &out, uint32_t deadlineUs) {
  uint8_t buf[10];
  const uint16_t evIrq = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR |
                         RADIOLIB_SX126X_IRQ_HEADER_ERR;
  for (;;) {
    const uint16_t irq = _r->getIrqStatus();
    if (irq & evIrq) {
      const bool crcErr = (irq & (RADIOLIB_SX126X_IRQ_CRC_ERR |
                                  RADIOLIB_SX126X_IRQ_HEADER_ERR)) != 0;
      const size_t len = _r->getPacketLength();   // 必须在 readData 之前
      _r->readData(buf, sizeof(buf));             // 读数据 + 清 IRQ
      if (!crcErr && len == 10 && tdmaDecode(buf, out)) return true;
      _r->startReceive();                         // 坏包/长度不符：丢弃并重新武装
    } else if ((int32_t)(deadlineUs - micros()) <= 0) {
      return false;
    }
    vTaskDelay(1);
  }
}

// 发射一帧（非阻塞起发 + 轮询 IRQ 寄存器等 TX_DONE）。
// 2026-09-10 修正（见 docs/lora-gateway-test.md §4.1）：原实现用 RadioLib 阻塞
// transmit()，它靠读 **DIO1 电平**判完成 —— 多射频网关板每个射频只有
// NSS/BUSY/RST 三根线（没有 DIO1），digitalRead(-1) 恒 0 → 每次发射白等
// timeout = 5×空口 ≈ 46ms 才返回 TX_TIMEOUT（数据其实已发出）。
// 改成轮询 IRQ 寄存器后：接没接 DIO1 都一样，耗时 ≈ 空口时长 9.3ms。
void TdmaMac::txFrame(const TdmaFrame &f) {
  uint8_t buf[10];
  tdmaEncode(f, buf);
  _r->standby();                       // 配置命令需 standby
  if (_r->startTransmit(buf, 10) != RADIOLIB_ERR_NONE) return;
  const uint32_t deadline = micros() + 150000;   // 150ms 兜底
  for (;;) {
    if (_r->getIrqStatus() & RADIOLIB_SX126X_IRQ_TX_DONE) break;
    if ((int32_t)(micros() - deadline) > 0) break;
  }
  _r->finishTransmit();                // 清 IRQ + standby
}

void TdmaMac::lockFromBeacon(const TdmaFrame &f, uint32_t rxEndUs) {
  _sfStartUs = rxEndUs - T_AIR;  // 估算信标 TX 起点 = 超帧起点
  _sfCounter = ((uint32_t)f.payload[0] << 24) | ((uint32_t)f.payload[1] << 16) |
               ((uint32_t)f.payload[2] << 8) | f.payload[3];
  _n = f.payload[4] & 0x1F;                    // 当前设备数（自适应）
  _beaconMapVer = (uint8_t)(f.payload[4] >> 5);  // 映射版本
  _locked = true;
}

void TdmaMac::pushRx(const TdmaFrame &f) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  ringPush(_rx, 16, _rxHead, _rxTail, f);
  xSemaphoreGive(_mutex);
}

void TdmaMac::pushDl(const TdmaFrame &f) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  ringPush(_dl, 32, _dlHead, _dlTail, f);
  xSemaphoreGive(_mutex);
}

// ===== 设备从模式 =====
// 负载均衡扫描：收集全部信道信标中的 N（该信道已注册设备数），
// score = N×100 + 随机 0-99 → 选 N 最小者（负载最低），N 相同时随机分散
// （多设备并发开机不扎堆同一信道）。选定后在目标信道听信标锁相。
bool TdmaMac::scanForBeacon() {
  uint8_t bestCh = 0xFF;
  uint16_t bestScore = 0xFFFF;
  for (uint8_t c = 0; c < _channelCount; c++) {
    _r->setFrequency(_channels[c]);
    _r->startReceive();
    TdmaFrame f;
    uint32_t t0 = micros();
    if (readPacketPoll(f, t0 + 250000) && f.type == TF_BEACON) {
      uint8_t n = f.payload[4] & 0x1F;  // 该信道当前设备数（信标广播）
      uint16_t score =
          (uint16_t)n * 100u + (uint16_t)(esp_random() % 100);
      if (score < bestScore) {
        bestScore = score;
        bestCh = c;
      }
    }
  }
  if (bestCh == 0xFF) return false;  // 未听到任何信标

  if (bestCh != _channel) {
    _channel = bestCh;
    _r->setFrequency(_channels[_channel]);
  }
  // 在选定信道听信标锁相（统一路径）
  _r->startReceive();
  TdmaFrame f;
  uint32_t t0 = micros();
  if (readPacketPoll(f, t0 + 250000) && f.type == TF_BEACON) {
    lockFromBeacon(f, micros());
    Serial.printf("[tdma] ch%u selected (balanced), N=%u ver=%u\n", _channel,
                  _n, _beaconMapVer);
    return true;
  }
  _locked = false;
  return false;
}

void TdmaMac::setJoinPayload(const uint8_t payload5[5], uint8_t flags) {
  if (payload5) memcpy(_joinPayload, payload5, 5);
  _joinFlags = flags;
}

void TdmaMac::tryJoin(uint32_t regStartUs) {
  if (_joinTries % 3 != 0) {  // 每 3 个超帧尝试一次，避免注册槽拥塞
    _joinTries++;
    return;
  }
  _joinTries++;
  uint32_t jitter = (uint32_t)esp_random() % 6000;  // 0-6ms 随机退避
  waitUntil(regStartUs + jitter);
  TdmaFrame j;
  tdmaMake(j, _devIdx, TF_JOIN, _joinFlags, 0, _joinPayload);
  txFrame(j);
  Serial.printf("[tdma] join try on ch%u\n", _channel);
}

void TdmaMac::runDevice() {
  if (!_locked) {
    if (scanForBeacon()) {
      // 首次锁定即尝试注册
    } else {
      return;
    }
  }

  // 等待下一超帧起点（提前 6ms 唤醒，末段忙等）
  int32_t toGo = (int32_t)(_sfStartUs - micros());
  if (toGo > 0) {
    if (toGo > 6000) vTaskDelay(pdMS_TO_TICKS((toGo - 6000) / 1000));
    waitUntil(_sfStartUs - 1500);  // 提前 1.5ms 武装接收
  }
  uint32_t sfStart = _sfStartUs;
  TdmaFrame f;

  // 1) 信标窗 RX：锁定/校正相位 + 刷新 N/mapVer
  _r->startReceive();
  if (readPacketPoll(f, sfStart + T_BE + 3000)) {
    if (f.type == TF_BEACON) {
      lockFromBeacon(f, micros());
    } else {
      pushRx(f);
    }
  }

  // 2) 广播下行窗 RX：最多取 1 帧（TF_ASSIGN 由 MAC 消费）
  //    射频仍在 RX（连续模式，未重武装），网关下行帧 [9.3, 18.6ms] 必然被捕获
  if (readPacketPoll(f, sfStart + T_BE + T_DL + 3000)) {
    if (f.type == TF_BEACON) {
      lockFromBeacon(f, micros());
    } else if (f.type == TF_ASSIGN && f.devIdx == _devIdx) {
      uint8_t newCh = f.payload[0];
      uint8_t newSlot = f.payload[1];
      uint8_t newN = f.payload[2];
      uint8_t newVer = f.payload[3];
      if (newSlot == 0xFF) {
        // 本信道已满：重新全信道负载均衡扫描（选其他最空信道）
        Serial.printf("[tdma] ch%u full, rescan\n", _channel);
        _assigned = false;
        _locked = false;
      } else {
        _slot = newSlot;
        _mapVer = newVer;
        _n = newN;
        _assigned = true;
        Serial.printf("[tdma] assigned ch%u slot%u N%u ver%u\n", _channel,
                      _slot, _n, _mapVer);
        if (_channel != newCh) {
          _channel = newCh;
          _r->setFrequency(_channels[_channel]);
          _locked = false;  // 换信道后重新锁相
        }
      }
    } else {
      pushRx(f);
    }
  }
  _r->standby();  // 省电：回 standby（~1.6mA），下超帧再醒

  // 3) 自身时隙 TX / 注册时隙补 JOIN
  bool canTx = _assigned && _slot < _n && _beaconMapVer == _mapVer;
  if (canTx && _txHead != _txTail) {
    TdmaFrame tx;
    bool got;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    got = ringPop(_tx, 8, _txHead, _txTail, tx);
    xSemaphoreGive(_mutex);
    if (got) {
      uint32_t slotStart = sfStart + T_BE + T_DL + (uint32_t)_slot * T_SLOT;
      waitUntil(slotStart);
      txFrame(tx);
    }
  } else if (!canTx) {
    uint32_t regStart = sfStart + T_BE + T_DL + (uint32_t)_n * T_SLOT;
    tryJoin(regStart);
  }

  _sfStartUs = sfStart + T_BE + T_DL + (uint32_t)_n * T_SLOT + T_REG;
}

// ===== 网关主模式 =====
void TdmaMac::onGwUplink(const TdmaFrame &f) {
  if (f.devIdx == TF_BROADCAST_IDX) return;
  uint32_t now = millis();
  // 已登记设备：刷新 liveness（重复 JOIN 不触发重排）
  for (uint8_t i = 0; i < _gwCount; i++) {
    if (_gw[i].valid && _gw[i].idx == f.devIdx) {
      _gw[i].lastSeenMs = now;
      return;
    }
  }
  // 新设备：仅接受 JOIN 注册
  if (f.type != TF_JOIN) return;
  if (_gwCount >= _maxSlots) {
    // 信道满：回 TF_ASSIGN(slot=0xFF)，设备试下一信道
    TdmaFrame a;
    tdmaMake(a, f.devIdx, TF_ASSIGN, 0, 0, NULL);
    a.payload[0] = _channel;
    a.payload[1] = 0xFF;
    a.payload[2] = _n;
    a.payload[3] = _mapVer;
    pushDl(a);
    Serial.printf("[tdma] ch%u full, reject %u\n", _channel, f.devIdx);
    return;
  }
  _gw[_gwCount].idx = f.devIdx;
  _gw[_gwCount].valid = true;
  _gw[_gwCount].lastSeenMs = now;
  _gwCount++;
  Serial.printf("[tdma] ch%u join dev=%u -> %u devices\n", _channel, f.devIdx,
                _gwCount);
  reSlot();
}

void TdmaMac::reSlot() {
  // 密集重排：注册序 → 时隙 0..count-1；mapVer 递增
  _mapVer++;
  _n = _gwCount > 0 ? _gwCount : 1;
  for (uint8_t i = 0; i < _gwCount; i++) _gw[i].slot = i;
  for (uint8_t i = 0; i < _gwCount; i++) {
    TdmaFrame a;
    tdmaMake(a, _gw[i].idx, TF_ASSIGN, 0, 0, NULL);
    a.payload[0] = _channel;
    a.payload[1] = _gw[i].slot;
    a.payload[2] = _n;
    a.payload[3] = _mapVer;
    pushDl(a);
  }
}

void TdmaMac::expireDevices() {
  bool changed = false;
  uint32_t now = millis();
  for (uint8_t i = 0; i < _gwCount; i++) {
    if (_gw[i].valid && (now - _gw[i].lastSeenMs) > DEV_TIMEOUT_MS) {
      Serial.printf("[tdma] ch%u expire dev=%u\n", _channel, _gw[i].idx);
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
  reSlot();
}

void TdmaMac::runGateway() {
  if (!_armed) {
    _sfStartUs = micros() + 10000;  // 首个超帧栅格
    _armed = true;
    return;
  }

  int32_t toGo = (int32_t)(_sfStartUs - micros());
  if (toGo > 0) {
    if (toGo > 5000) vTaskDelay(pdMS_TO_TICKS((toGo - 5000) / 1000));
    waitUntil(_sfStartUs);
  }

  // 1) 信标 TX（含 N 与 mapVer，自适应超帧长度）
  TdmaFrame b;
  tdmaMake(b, TF_BROADCAST_IDX, TF_BEACON, 0, 0, NULL);
  uint32_t c = _sfCounter++;
  b.payload[0] = (uint8_t)(c >> 24);
  b.payload[1] = (uint8_t)(c >> 16);
  b.payload[2] = (uint8_t)(c >> 8);
  b.payload[3] = (uint8_t)c;
  b.payload[4] = (uint8_t)((_mapVer << 5) | (_n & 0x1F));
  // 相位基准：**实际信标空口起点**（设备端 lockFromBeacon 也是以信标空口起点
  // 为准的），不再用本地预定时刻 sfStart —— 这样两端相位天然一致。
  const uint32_t bcStart = micros();
  txFrame(b);

  // 2) 广播下行窗：信标发完紧接着发 1 帧下行（[9.3, 18.6ms]，早结束
  //    以留足上行 RX 武装时间，设备连续 RX 必然捕获）
  if (_dlHead != _dlTail) {
    TdmaFrame dl;
    bool got;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    got = ringPop(_dl, 32, _dlHead, _dlTail, dl);
    xSemaphoreGive(_mutex);
    if (got) {
      txFrame(dl);
    }
  }

  // 3) 设备时隙 0..N-1 共听（武装一次后连续 RX，槽间不重武装）
  _r->startReceive();
  for (uint8_t k = 0; k < _n; k++) {
    uint32_t sStart = bcStart + T_BE + T_DL + (uint32_t)k * T_SLOT;
    uint32_t sEnd = sStart + T_SLOT + T_SLOT_RX_MARGIN;   // 取包余量
    TdmaFrame f;
    if (readPacketPoll(f, sEnd)) {
      onGwUplink(f);
      pushRx(f);
    }
  }

  // 4) 注册时隙（新设备 JOIN / 失配设备补 JOIN）
  uint32_t regStart = bcStart + T_BE + T_DL + (uint32_t)_n * T_SLOT;
  TdmaFrame f;
  if (readPacketPoll(f, regStart + T_REG + T_REG_RX_MARGIN)) {
    onGwUplink(f);
    pushRx(f);
  }
  _r->standby();

  // 5) 心跳超时清理 → 触发重排（mapVer 递增）
  expireDevices();

  // 6) 下一个超帧：以实际信标起点为基准、**周期恒定** = 30+10N + T_SF_TAIL
  //    （不能按标称 30+10N 递推：注册窗会超时，误差逐帧累积 → 设备失锁）
  _sfStartUs = bcStart + T_BE + T_DL + (uint32_t)_n * T_SLOT + T_REG + T_SF_TAIL;
}