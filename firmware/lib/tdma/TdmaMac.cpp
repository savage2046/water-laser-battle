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

// ===== 下行容量保护（2026-09-13，任务 1）=====
// 下行窗每超帧只有 1 帧的容量（10ms）。补发 ASSIGN 的每设备最小间隔：
// 50 台若各每 2s 补发一次 = 25 次/s ✗ 远超排水能力。配合 onGwUplink 的
// "只对注册窗 JOIN 补发"，把补发率限死在 ≤1 帧/超帧 = 下行容量本身。
static const uint32_t ASSIGN_RESEND_MIN_MS = 1000;
// ===== 注册窗自适应（2026-09-13，任务 2）=====
// 有新设备注册后把注册窗临时开到 REG_SLOTS_BURST 个子槽（提高冷启动收敛速度），
// 静默 REG_BURST_HOLD_MS 后缩回 1 个子槽（保证稳态超帧仍是 37+10N ms）。
// 子槽数通过信标 payload[4] 广播给设备，两端据此算超帧长度。
static const uint8_t  REG_SLOTS_BURST = 3;
static const uint32_t REG_BURST_HOLD_MS = 5000;

// MAC 任务栈（字）。单射频 4096 足够；多射频（T3 目标 3 射频）须实测降至 2048-3072
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
// fine=true：用 200µs 粒度轮询（vTaskDelay 是 1ms 粒度，足以错过 2.1ms 的前导）。
// 设备端下行窗必须用 fine —— 见 runDevice 里 2026-09-13 的说明。
bool TdmaMac::readPacketPoll(TdmaFrame &out, uint32_t deadlineUs, bool fine) {
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
    if (fine) {
      delayMicroseconds(200);
    } else {
      vTaskDelay(1);
    }
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
  if (_r->startTransmit(buf, 10) != RADIOLIB_ERR_NONE) {
    // ⚠️ 2026-09-13：原实现静默返回 —— 帧已经被 ringPop 出队，一旦起发失败就永久丢失
    //    （表现为"网关日志说补发了 ASSIGN，设备永远收不到"）。这里至少打出来。
    Serial.println("[tdma] txFrame: startTransmit 失败，该帧丢失");
    return;
  }
  const uint32_t deadline = micros() + 150000;   // 150ms 兜底
  for (;;) {
    if (_r->getIrqStatus() & RADIOLIB_SX126X_IRQ_TX_DONE) break;
    if ((int32_t)(micros() - deadline) > 0) break;
  }
  _r->finishTransmit();                // 清 IRQ + standby
}

void TdmaMac::lockFromBeacon(const TdmaFrame &f, uint32_t rxEndUs) {
  const uint8_t flags = f.payload[4];
  if ((uint8_t)(flags >> 2) != TDMA_PROTO_VER) return;   // 版本不符：不锁相（已在扫描时告警）
  _sfStartUs = rxEndUs - T_AIR;  // 估算信标 TX 起点 = 超帧起点
  // 信标 payload 布局（2026-09-13 起，**改动必须递增 TDMA_PROTO_VER**）：
  //   payload[0..2] = 超帧计数器（24 位；5 超帧/s 也要 38 天才回绕）
  //   payload[3]    = (mapVer << 5) | (N & 0x1F)
  //   payload[4]    = (协议版本 << 2) | (注册窗子槽数 & 0x03)
  _sfCounter = ((uint32_t)f.payload[0] << 16) | ((uint32_t)f.payload[1] << 8) |
               f.payload[2];
  _n = f.payload[3] & 0x1F;                      // 当前设备数（自适应）
  _beaconMapVer = (uint8_t)(f.payload[3] >> 5);  // 映射版本
  _regSlots = (uint8_t)(flags & 0x03);           // 注册窗子槽数（算超帧长度用）
  if (_regSlots == 0) _regSlots = 1;
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
// 单频点停留窗：真网关超帧 ≈40~200ms，250ms 内应能听到 ≥2 个信标。
#define TDMA_SCAN_DWELL_US 250000

// 在指定频点停留一个窗，统计"信标证据"：
//   nOut=信标里的 N（该信道已注册设备数）｜countOut=窗内信标个数
//   ctrOkOut=相邻两信标的超帧计数器是否**逐一递增**（真网关的硬特征）
//
// ⚠️ 2026-09-13 加固（实测踩坑）：原实现"取到第一帧就换频点"，且只按 N 打分
//    （`score = N*100 + random(0..99)`）。当某频点存在**能解出 TF_BEACON 的强信号**时
//    （实测 504MHz 处 −36dBm），它的 N 往往也是 1 → score 与真网关**同档** → 纯随机
//    决胜负。实测后果：枪端锁到 ch17(504MHz) 的伪信标，在错误频点一直发 JOIN，而网关
//    在 ch1(472MHz) → 永远注册不上（枪端日志 `ch17 selected (balanced), N=1 ver=0`
//    之后一直是 `join try on ch17`、`reg=no`；网关侧什么都收不到）。
//    现在只有"窗内 ≥2 个信标且计数器逐一递增"的频点才**确证**为本网网关。
bool TdmaMac::dwellBeacon(uint8_t c, uint8_t &nOut, uint8_t &countOut,
                          bool &ctrOkOut, uint32_t &ctrOut) {
  _r->setFrequency(_channels[c]);
  _r->startReceive();
  const uint32_t deadline = micros() + TDMA_SCAN_DWELL_US;
  uint8_t cnt = 0;
  bool ctrOk = false;
  uint32_t prev = 0;
  bool havePrev = false;
  nOut = 0;
  ctrOut = 0;
  for (;;) {
    TdmaFrame f;
    if (!readPacketPoll(f, deadline)) break;   // 窗内不再有帧
    if (f.type != TF_BEACON) continue;         // 非信标：继续等，不能就此换频点
    // 协议版本校验：不匹配就不计入（否则会拿错位的字节算出假证据）
    if ((uint8_t)(f.payload[4] >> 2) != TDMA_PROTO_VER) {
      if (!_verWarned) {
        _verWarned = true;
        Serial.printf("[tdma] ❌ 信标协议版本不匹配：收到 ver=%u，本机 ver=%u —— "
                      "对端固件版本不同（网关/枪端请一并重烧）！\n",
                      (unsigned)(f.payload[4] >> 2), (unsigned)TDMA_PROTO_VER);
      }
      continue;
    }
    // ⚠️ 计数器是**3 字节**（payload[0..2]，2026-09-13 起）。此处曾漏改仍按 4 字节读，
    //    导致值 ≈ `计数器<<8`、相邻信标差 256 而非 1 → "确证"永远失败。
    const uint32_t ctr = ((uint32_t)f.payload[0] << 16) |
                         ((uint32_t)f.payload[1] << 8) | f.payload[2];
    // "递增且跨度小"即算连续：**不能要求恰好 +1**（漏掉 1~2 个信标很常见，实测就因此
    // 把真网关判成"未确证"）。跨度 ≤16 也足以排除无关杂散/别家信号。
    if (havePrev && ctr > prev && (ctr - prev) <= 16) ctrOk = true;
    prev = ctr;
    havePrev = true;
    nOut = (uint8_t)(f.payload[3] & 0x1F);   // N 在 payload[3] 低 5 位
    cnt++;
  }
  countOut = cnt;
  ctrOkOut = ctrOk;
  ctrOut = prev;   // 末值：不同网关各自独立计数 → 值差异大 = 不同源
  return cnt > 0;
}

// 注册无果 → 改试下一个候选信道（自愈，2026-09-13）。
// 触发点：① runDevice 里"锁定后 2.5s 仍未收到 TF_ASSIGN"（对端不是本网网关，
//            或 ASSIGN 一直被错过）；② 收到 slot=0xFF（网关说本信道已满）。
// 没有其它候选时退回全表扫描（_preferCh=0xFF）。
void TdmaMac::advanceCandidate(const char *why) {
  if (_candCount >= 2) {
    _candIdx = (uint8_t)((_candIdx + 1) % _candCount);
    _preferCh = _candCh[_candIdx];
  } else {
    _preferCh = 0xFF;
  }
  _locked = false;
  _assigned = false;
  _joinWaitStartMs = millis();
  Serial.printf("[tdma] 放弃 ch%u（%s）→ ", (unsigned)_channel, why);
  if (_preferCh == 0xFF) {
    Serial.println("重新全表扫描");
  } else {
    Serial.printf("改试候选 ch%u（%u/%u）\n", (unsigned)_preferCh,
                  (unsigned)(_candIdx + 1), (unsigned)_candCount);
  }
}

// 负载均衡扫描：**先确证本网网关**，再按 N 择优 —— score = N×100 + 随机 0-99
//（选 N 最小者=负载最低，N 相同时随机分散，避免多设备并发开机扎堆同一信道）。
// 选定后在目标信道听信标锁相。
//
// ⚠️ 2026-09-13：额外产出**候选信道列表** `_candCh[]`（确证优先、其次 score 小者优先）。
//    锁定后若迟迟收不到 TF_ASSIGN（见 runDevice 超时判断 / slot=0xFF），就依次改试下一个
//    —— "锁到了伪信标或别人的网关"因此不再是死路（原实现只会对着它一直发 JOIN）。
bool TdmaMac::scanForBeacon() {
  struct Hit {
    uint8_t ch;
    uint16_t score;
    bool ok;   // 确证：本频点 ≥2 个信标且超帧计数器递增
  };
  Hit hits[TDMA_STD_CHANNELS];
  uint8_t hitCount = 0;
  bool chosen = false;

  // 若刚被指定换信道（ASSIGN 指名 / 注册自愈重试）：**先只试那个频点**，
  // 不要重新全表随机选一次（原实现换信道后走全表扫描，可能又选回伪信标 → 死循环）
  if (_preferCh != 0xFF && _preferCh < _channelCount) {
    uint8_t n = 0, cnt = 0;
    bool ctr = false;
    uint32_t ctrVal = 0;
    const bool heard = dwellBeacon(_preferCh, n, cnt, ctr, ctrVal);
    Serial.printf("[tdma] scan prefer-ch%u: %u beacon(s), counter=%s(sf=%lu)%s\n",
                  (unsigned)_preferCh, (unsigned)cnt, ctr ? "ok" : "NO",
                  (unsigned long)ctrVal,
                  (heard && cnt < 2) ? "（只 1 个信标，按重试目标接受）" : "");
    if (heard) {
      // 重试目标：听到信标就接受（确证与否都试一次，反正收不到 ASSIGN 会继续换）
      hits[hitCount].ch = _preferCh;
      hits[hitCount].score = (uint16_t)n * 100u + 50u;
      hits[hitCount].ok = (cnt >= 2 && ctr);
      hitCount++;
      chosen = true;
    }
    _preferCh = 0xFF;
  }

  if (!chosen) {
    for (uint8_t c = 0; c < _channelCount; c++) {
      uint8_t n = 0, cnt = 0;
      bool ctr = false;
      uint32_t ctrVal = 0;
      if (!dwellBeacon(c, n, cnt, ctr, ctrVal)) continue;
      Serial.printf("[tdma] scan ch%u(%.1fMHz): %u beacon(s), counter=%s(sf=%lu), N=%u\n",
                    (unsigned)c, (double)_channels[c], (unsigned)cnt,
                    ctr ? "ok" : "NO", (unsigned long)ctrVal, (unsigned)n);
      if (hitCount < TDMA_STD_CHANNELS) {
        hits[hitCount].ch = c;
        hits[hitCount].score =
            (uint16_t)n * 100u + (uint16_t)(esp_random() % 100);
        hits[hitCount].ok = (cnt >= 2 && ctr);
        hitCount++;
      }
    }
  }

  if (hitCount == 0) return false;   // 一个信标都没听到

  // 排序：确证优先；同级按 score 升序（N 小=负载低者优先，同 N 随机分散）
  for (uint8_t i = 0; i + 1 < hitCount; i++) {
    for (uint8_t j = 0; j + 1 < hitCount - i; j++) {
      const bool a = hits[j].ok, b = hits[j + 1].ok;
      if ((!a && b) || (a == b && hits[j + 1].score < hits[j].score)) {
        Hit t = hits[j];
        hits[j] = hits[j + 1];
        hits[j + 1] = t;
      }
    }
  }

  // 存候选（最多 4 个）供"注册无果时改试下一个"
  _candCount = (hitCount > 4) ? 4 : hitCount;
  for (uint8_t i = 0; i < _candCount; i++) _candCh[i] = hits[i].ch;
  _candIdx = 0;
  Serial.print("[tdma] 候选信道: ");
  for (uint8_t i = 0; i < _candCount; i++) {
    Serial.printf("ch%u%s%s", (unsigned)hits[i].ch,
                  hits[i].ok ? "(确证)" : "(未确证)",
                  (i + 1 < _candCount) ? " > " : "\n");
  }
  if (_candCount == 1 && !hits[0].ok)
    Serial.println("[tdma] warn: 唯一候选未确证（可能不是本网网关，"
                   "若注册不上请查是否有别的同参数信标/网关）");

  const uint8_t bestCh = hits[0].ch;
  if (bestCh != _channel) {
    _channel = bestCh;
    _r->setFrequency(_channels[_channel]);
  }
  // 在选定信道听信标锁相（统一路径）
  _r->startReceive();
  TdmaFrame f;
  uint32_t t0 = micros();
  if (readPacketPoll(f, t0 + TDMA_SCAN_DWELL_US) && f.type == TF_BEACON) {
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

// 注册窗内发一帧 TF_JOIN（**真·时隙 ALOHA**，2026-09-13 改造，任务 2）
//
// 原实现："每 3 个超帧试一次" + 0~6ms 抖动 —— 多台设备同时开机时是**同一个确定性
// 节拍**，会挤进同一个 10ms 注册窗撞成一团；按时隙 ALOHA 估算 G≈5.7 时每窗成功率仅
// ~0.02（`S = G·e^-G`），50 台冷启动收敛要分钟级。
// 现在：① 每次尝试后重抽一个 **1..8 超帧的随机等待**（把同步风暴打散成独立到达）；
//       ② 在注册窗内**随机选一个子槽**（子槽数由信标的 `_regSlots` 给出），
//          子槽内再加 0~0.5ms 抖动（帧 9.28ms < 子槽 10ms，抖动必须远小于子槽）。
void TdmaMac::tryJoin(uint32_t regStartUs) {
  if (_joinCountdown > 0) {      // 还没轮到本次尝试
    _joinCountdown--;
    return;
  }
  _joinCountdown = (uint8_t)(1 + (esp_random() % 8));   // 下次尝试再等 1..8 个超帧

  const uint8_t slots = (_regSlots == 0) ? 1 : _regSlots;
  const uint8_t sub = (uint8_t)(esp_random() % slots);
  const uint32_t jitter = (uint32_t)(esp_random() % 500);
  waitUntil(regStartUs + (uint32_t)sub * T_SLOT + jitter);
  TdmaFrame j;
  tdmaMake(j, _devIdx, TF_JOIN, _joinFlags, 0, _joinPayload);
  txFrame(j);
  Serial.printf("[tdma] join try on ch%u (sub%u/%u)\n", _channel, (unsigned)sub,
                (unsigned)slots);
}

// 处理一份发给本机的 TF_ASSIGN（信标窗尾 / 下行窗 两处共用；2026-09-13 抽出）
void TdmaMac::applyAssign(const TdmaFrame &f) {
  const uint8_t newCh = f.payload[0];
  const uint8_t newSlot = f.payload[1];
  const uint8_t newN = f.payload[2];
  const uint8_t newVer = f.payload[3];
  if (newSlot == 0xFF) {
    // 本信道已满：换下一个候选信道（advanceCandidate 会置 _locked=false/_preferCh；
    // 没有其它候选时退回全表扫描）
    Serial.printf("[tdma] ch%u full\n", _channel);
    advanceCandidate("本信道已满");
    return;
  }
  _slot = newSlot;
  _mapVer = newVer;
  _n = newN;
  _assigned = true;
  Serial.printf("[tdma] assigned ch%u slot%u N%u ver%u\n", _channel, _slot, _n,
                _mapVer);
  if (_channel != newCh) {
    // 这条分支会把 _locked 置假 → 重新锁相；_preferCh 保证重扫时**只试这个信道**
    // （否则全表扫描可能又选回伪信标/别的网关 → 死循环）
    Serial.printf("[tdma] ASSIGN ch=%u 与本地 _channel=%u 不一致 → 重新锁相\n",
                  newCh, _channel);
    _channel = newCh;
    _r->setFrequency(_channels[_channel]);
    _preferCh = newCh;
    _locked = false;
  }
}

void TdmaMac::runDevice() {
  if (!_locked) {
    if (scanForBeacon()) {
      _joinWaitStartMs = millis();   // 新信道：开始等 TF_ASSIGN
    } else {
      return;
    }
  }

  // 注册自愈（2026-09-13）：锁定后 2.5s 仍没收到 TF_ASSIGN —— 说明对端不是本网网关
  //   （锁到伪信标 / 别人的网关），或 ASSIGN 一直被错过 → 换下一个候选信道重试。
  //   正常情况 ASSIGN 在 1~2 个超帧内到达（远小于 2.5s），不会误触发。
  if (!_assigned && _candCount > 1 &&
      (uint32_t)(millis() - _joinWaitStartMs) > 2500) {
    advanceCandidate("2.5s 未收到 ASSIGN");
    return;
  }

  // 等待下一超帧起点（提前 6ms 唤醒，末段忙等）
  int32_t toGo = (int32_t)(_sfStartUs - micros());
  if (toGo > 0) {
    if (toGo > 6000) vTaskDelay(pdMS_TO_TICKS((toGo - 6000) / 1000));
    waitUntil(_sfStartUs - 1500);  // 提前 1.5ms 武装接收
  }
  uint32_t sfStart = _sfStartUs;
  TdmaFrame f;

  // 1) 信标窗 RX + 2) 广播下行窗 RX —— **两窗之间必须重新武装 RX**（2026-09-13 关键修正）
  //
  // ⚠️ 实测依据（COM14 网关日志）：网关每次都收到 JOIN 并立刻补发 TF_ASSIGN
  //    （`[tdma] ch1 re-assign …` 连续刷屏、`devs=1` 稳定），而设备端 30 秒内一次都没进
  //    `assigned` —— **上行通、下行全丢**。
  //    原因：原实现把两窗当成两次独立轮询，step 1 读完信标后**没有重新武装 RX**，
  //    而 RX_DONE 之后芯片并不保证仍留在 RX（原注释"连续模式未重武装必然捕获"是错的假设）。
  //    网关下行帧在信标结束后 ~1ms 就发，前导只有 4 符号（SF7/BW500k ≈ 2.1ms），
  //    错过前导即整帧丢失；再加上轮询粒度 1ms（vTaskDelay(1)）足以错过。
  //    修法：① 读完信标**立刻重武装**；② 下行窗用 fine=true（200µs 粒度）。
  _r->startReceive();
  if (readPacketPoll(f, sfStart + T_BE + 3000, true)) {   // fine：尽快读到信标 → 相位更准、留足下行窗时间
    if (f.type == TF_BEACON) {
      lockFromBeacon(f, micros());
    } else if (f.type == TF_ASSIGN && f.devIdx == _devIdx) {
      applyAssign(f);   // ASSIGN 落在信标窗尾（设备起步晚）也一并消费，别丢进 RX 队列
    } else {
      Serial.printf("[tdma] 信标窗收到 type=0x%02X devIdx=%u seq=%u\n",
                    (unsigned)f.type, (unsigned)f.devIdx, (unsigned)f.seq);
      pushRx(f);
    }
  }

  // 采用**信标校正后的相位**：lockFromBeacon 已把 _sfStartUs 设为"本超帧真实起点"。
  // 若仍沿用帧首那个本地推算值，本帧的时隙/注册窗就按上一帧的推算走，晶振漂移会逐帧
  // 累积（实测 5 分钟内心跳序号已有明显丢包，见 docs/lora-联调记录-2026-09-13.md）。
  sfStart = _sfStartUs;

  _r->startReceive();   // ← 关键：下行窗前重新武装（见上方说明）

  // 2) 广播下行窗 RX：最多取 1 帧（TF_ASSIGN 由 MAC 消费）
  if (readPacketPoll(f, sfStart + T_BE + T_DL + 3000, true)) {
    if (f.type == TF_BEACON) {
      lockFromBeacon(f, micros());
    } else if (f.type == TF_ASSIGN && f.devIdx == _devIdx) {
      applyAssign(f);
    } else {
      // 诊断：能收到"别人的帧"就说明下行链路是通的（RX 确实武装上了）
      Serial.printf("[tdma] DL窗收到 type=0x%02X devIdx=%u seq=%u（非本机 ASSIGN）\n",
                    (unsigned)f.type, (unsigned)f.devIdx, (unsigned)f.seq);
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

  _sfStartUs = sfStart + T_BE + T_DL + (uint32_t)_n * T_SLOT +
               (uint32_t)_regSlots * T_REG + T_SF_TAIL;
}

// ===== 网关主模式 =====
// 网关侧：收到一帧上行。fromRegSlot=true 表示这帧来自**注册窗**（设备还没时隙）。
//
// ⚠️ 2026-09-13 下行容量改造（任务 1）：
//   原先"任何重复 JOIN 都补发一次 ASSIGN"——但设备侧有两类 JOIN：
//     ① MAC 级 JOIN：设备**没拿到** ASSIGN（走注册窗，因为它没有时隙）；
//     ② 应用级 JOIN：设备已注册，只是还没收到服务器的 W 帧，在**它自己的时隙**里重发 J
//        （`gun/main.cpp` 的 `REJOIN_MS 5000`）。这类 JOIN 补发 ASSIGN **毫无意义**。
//   若两者都补发，50 台 × 每 5s = 10 次/s 的 ASSIGN 会远超下行窗排水能力
//   （每超帧只发 1 帧 = N=17 时 5 帧/s）→ W/S/E 等下行帧会被饿死。
//   现在：**只有来自注册窗的 JOIN 才补发**，而注册窗每超帧最多进 1 帧 →
//   补发率天然被限死在"每超帧 1 帧 = 下行容量本身"，不会挤占其它下行帧 ✓
//   （外加每设备 ASSIGN_RESEND_MIN_MS 的节流兜底）。
void TdmaMac::onGwUplink(const TdmaFrame &f, bool fromRegSlot) {
  if (f.devIdx == TF_BROADCAST_IDX) return;
  uint32_t now = millis();
  // 已登记设备：刷新 liveness（重复 JOIN 不触发重排）
  for (uint8_t i = 0; i < _gwCount; i++) {
    if (_gw[i].valid && _gw[i].idx == f.devIdx) {
      _gw[i].lastSeenMs = now;
      if (fromRegSlot && f.type == TF_JOIN &&
          (uint32_t)(now - _gw[i].lastAssignMs) > ASSIGN_RESEND_MIN_MS) {
        // 设备在注册窗里 JOIN = 它手上没有可用时隙 → 补发一份 ASSIGN
        sendAssign(i);
        Serial.printf("[tdma] ch%u re-assign dev=%u slot=%u N=%u ver=%u "
                      "(注册窗重复 JOIN，补发 ASSIGN)\n",
                      _channel, _gw[i].idx, _gw[i].slot, _n,
                      (unsigned)(_mapVer & 0x07));
      }
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
    a.payload[3] = (uint8_t)(_mapVer & 0x07);   // 与信标的 3 bit 对齐（见 sendAssign）
    pushDl(a);
    Serial.printf("[tdma] ch%u full, reject %u\n", _channel, f.devIdx);
    return;
  }
  _gw[_gwCount].idx = f.devIdx;
  _gw[_gwCount].valid = true;
  _gw[_gwCount].lastSeenMs = now;
  _gw[_gwCount].lastAssignMs = now;
  _gwCount++;
  // 有新设备注册 → 打开注册突发窗（多给两个子槽，让其余设备更快注册完）
  _regBurstUntilMs = millis() + REG_BURST_HOLD_MS;
  Serial.printf("[tdma] ch%u join dev=%u -> %u devices\n", _channel, f.devIdx,
                _gwCount);
  reSlot();
}

// 组一份 TF_ASSIGN 并压入下行队列（reSlot 与"设备重复 JOIN 时补发"两处共用）。
// ⚠️ payload[3] 必须用**低 3 位**的 mapVer：信标里 mapVer 只有 3 bit
//    （本文件末尾 `b.payload[4] = (uint8_t)((_mapVer << 5) | (_n & 0x1F))`），
//    而设备端把信标解出来的 `_beaconMapVer` 与 ASSIGN 里的 `_mapVer` **直接比较**
//    （runDevice 的 `canTx = ... && _beaconMapVer == _mapVer`）→ 发整个字节会在
//    `_mapVer >= 8` 时永远对不上，设备就永久注册不上（潜伏 bug，2026-09-13 一并修）。
void TdmaMac::sendAssign(uint8_t gwIdx) {
  TdmaFrame a;
  tdmaMake(a, _gw[gwIdx].idx, TF_ASSIGN, 0, 0, NULL);
  a.payload[0] = _channel;
  a.payload[1] = _gw[gwIdx].slot;
  a.payload[2] = _n;
  a.payload[3] = (uint8_t)(_mapVer & 0x07);
  _gw[gwIdx].lastAssignMs = millis();   // 补发节流用（见 onGwUplink）
  pushDl(a);
}

void TdmaMac::reSlot() {
  // 密集重排：注册序 → 时隙 0..count-1；mapVer 递增
  _mapVer++;
  _n = _gwCount > 0 ? _gwCount : 1;
  for (uint8_t i = 0; i < _gwCount; i++) _gw[i].slot = i;
  for (uint8_t i = 0; i < _gwCount; i++) sendAssign(i);
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
  //    注册窗子槽数也在这里决定并广播：有新设备注册后 5s 内开 3 个子槽，否则 1 个
  //    （2026-09-13，任务 2；两端据此算出相同的超帧长度）
  _regSlots = ((int32_t)(_regBurstUntilMs - millis()) > 0) ? REG_SLOTS_BURST : 1;
  TdmaFrame b;
  tdmaMake(b, TF_BROADCAST_IDX, TF_BEACON, 0, 0, NULL);
  uint32_t c = _sfCounter++;
  b.payload[0] = (uint8_t)(c >> 16);
  b.payload[1] = (uint8_t)(c >> 8);
  b.payload[2] = (uint8_t)c;
  b.payload[3] = (uint8_t)((_mapVer << 5) | (_n & 0x1F));
  // payload[4]：高 6 位 = 协议版本（设备据此拒绝解读不同版本的信标，见 TdmaProto.h）
  //              低 2 位 = 注册窗子槽数（1 或 3）
  b.payload[4] = (uint8_t)((TDMA_PROTO_VER << 2) | (_regSlots & 0x03));
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
      onGwUplink(f, false);   // 来自设备时隙（设备已有时隙）→ 不补发 ASSIGN，见 onGwUplink
      pushRx(f);
    }
  }

  // 4) 注册窗（1~3 个子槽，见 _regSlots）：新设备 JOIN / 失配设备补 JOIN
  //    每个子槽 = 一帧容量（10ms，帧 9.28ms + 抖动余量）。设备在子槽内随机选一个。
  uint32_t regStart = bcStart + T_BE + T_DL + (uint32_t)_n * T_SLOT;
  for (uint8_t s = 0; s < _regSlots; s++) {
    TdmaFrame f;
    uint32_t subEnd = regStart + (uint32_t)(s + 1) * T_SLOT + 2000;
    if (readPacketPoll(f, subEnd)) {
      onGwUplink(f, true);    // 来自注册窗 → 必要时补发 ASSIGN
      pushRx(f);
    }
  }
  _r->standby();

  // 5) 心跳超时清理 → 触发重排（mapVer 递增）
  expireDevices();

  // 6) 下一个超帧：以实际信标起点为基准、**周期恒定** = 30+10N + T_SF_TAIL
  //    （不能按标称 30+10N 递推：注册窗会超时，误差逐帧累积 → 设备失锁）
  _sfStartUs = bcStart + T_BE + T_DL + (uint32_t)_n * T_SLOT +
               (uint32_t)_regSlots * T_REG + T_SF_TAIL;
}