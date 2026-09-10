#include "Stats.h"
#include <math.h>
#include <string.h>

// ============================================================================
// 统计实现（见 Stats.h 的并发说明）
// ============================================================================

static DevStat      s_dev[MAX_DEVS];
static uint16_t     s_devCount = 0;
static GlobalStat   s_g;

// Ping 在途表：seq → 发送时刻（用于算 RTT）
struct PingOut { uint16_t seq; uint32_t us; bool used; };
static PingOut s_pingOut[32];

// 排序用临时缓冲（避免在栈上放 512B）
static uint16_t s_sortBuf[MAX_RTT_SAMPLES];

GlobalStat &statsG() { return s_g; }

// ---------------------------------------------------------------- 内部
// 缺口长度 → 分布桶：1 / 2 / 3 / 4 / 5-9 / ≥10
static uint8_t lossBucket(uint32_t g) {
  if (g <= 1) return 0;
  if (g == 2) return 1;
  if (g == 3) return 2;
  if (g == 4) return 3;
  if (g <= 9) return 4;
  return 5;
}

static void initDev(DevStat &d, uint8_t idx) {
  memset(&d, 0, sizeof(d));
  d.used = true;
  d.idx = idx;
  d.slot = 0xFF;
  d.rssiMin = 32767;
  d.rssiMax = -32768;
  d.snrMinX4 = 127;
  d.snrMaxX4 = -128;
  d.intMin = 0xFFFFFFFFu;
  d.offMin = 0x7FFFFFFF;
  d.offMax = -0x7FFFFFFF;
  d.respMin = 0xFFFFFFFFu;
  d.feMin = 32767;
  d.feMax = -32768;
}

// ---------------------------------------------------------------- API
DevStat *statsDev(uint8_t idx) {
  for (uint16_t i = 0; i < s_devCount; i++) {
    if (s_dev[i].used && s_dev[i].idx == idx) return &s_dev[i];
  }
  if (s_devCount >= MAX_DEVS) return NULL;
  DevStat &d = s_dev[s_devCount];
  initDev(d, idx);
  s_devCount++;
  return &d;
}

DevStat *statsDevAt(uint16_t i) {
  return (i < s_devCount) ? &s_dev[i] : NULL;
}

uint16_t statsDevUsed() { return s_devCount; }

void statsReset() {
  for (uint16_t i = 0; i < MAX_DEVS; i++) {
    initDev(s_dev[i], 0);
    s_dev[i].used = false;
  }
  s_devCount = 0;
  memset(&s_g, 0, sizeof(s_g));
  for (uint8_t k = 0; k < TDMA_STD_CHANNELS; k++) s_g.freq[k].mx = -200;
  s_g.airUsMin = 0xFFFFFFFFu;
  s_g.sfMin = 0xFFFFFFFFu;
  s_g.ping.rttMin = 0xFFFF;
  for (uint8_t i = 0; i < 32; i++) s_pingOut[i].used = false;
}

void statsCountRx(uint8_t kind) {
  switch (kind) {
    case 0: s_g.rxOk++; break;
    case 1: s_g.rxCrcErr++; break;
    case 2: s_g.rxLenErr++; break;
    default: s_g.rxCrc8Err++; break;
  }
}

void statsOnTx(uint32_t airUs, bool ok) {
  s_g.txFrames++;
  if (!ok) {
    s_g.txErr++;
    return;
  }
  if (airUs < s_g.airUsMin) s_g.airUsMin = airUs;
  if (airUs > s_g.airUsMax) s_g.airUsMax = airUs;
  s_g.airUsSum += airUs;
  s_g.airUsN++;
}

void statsOnFrame(const TdmaFrame &f, int16_t rssi, float snr, int16_t freqErrHz,
                  uint32_t nowMs, bool slotFrame, int32_t offUs, uint32_t respUs) {
  DevStat *d = statsDev(f.devIdx);
  if (!d) return;

  d->frames++;
  if (f.type < 32) d->byType[f.type]++;

  // J 帧 payload = 5B deviceId ASCII
  if (f.type == TF_JOIN) {
    memcpy(d->devId, f.payload, 5);
    d->devId[5] = 0;
    if (f.flags & TF_FLAG_HELMET) {
      // 头盔附属设备：名字里标注一下
      d->devId[5] = 'H';
      d->devId[6] = 0;
    }
  }

  // RSSI / SNR
  const int8_t s4 = (int8_t)lroundf(snr * 4.0f);
  if (!d->hasRf) {
    d->hasRf = true;
    d->rssiMin = d->rssiMax = rssi;
    d->snrMinX4 = d->snrMaxX4 = s4;
  } else {
    if (rssi < d->rssiMin) d->rssiMin = rssi;
    if (rssi > d->rssiMax) d->rssiMax = rssi;
    if (s4 < d->snrMinX4) d->snrMinX4 = s4;
    if (s4 > d->snrMaxX4) d->snrMaxX4 = s4;
  }
  d->rssiSum += rssi;
  d->snrSumX4 += s4;

  // 载频偏差（Hz）
  if (!d->hasFe) {
    d->hasFe = true;
    d->feMin = d->feMax = freqErrHz;
  } else {
    if (freqErrHz < d->feMin) d->feMin = freqErrHz;
    if (freqErrHz > d->feMax) d->feMax = freqErrHz;
  }
  d->feSum += freqErrHz;
  if (d->feN < 0xFFFF) d->feN++;

  // 到达间隔
  if (d->intSeen) {
    const uint32_t dt = nowMs - d->lastArrMs;
    if (dt < d->intMin) d->intMin = dt;
    if (dt > d->intMax) d->intMax = dt;
    d->intSum += dt;
    if (d->intN < 0xFFFF) d->intN++;
  }
  d->intSeen = true;
  d->lastArrMs = nowMs;

  // 上行 seq 连续性（J 帧 seq 恒 0；测试帧 PING/PONG 的 seq 复用，均不参与 PER）
  if (f.type != TF_JOIN && f.type != TF_TEST_PING && f.type != TF_TEST_PONG &&
      f.seq != 0) {
    d->seqRecv++;
    if (!d->seqSeen) {
      d->seqSeen = true;
      d->seqFirst = f.seq;
    } else {
      const uint16_t delta = (uint16_t)(f.seq - d->seqPrev);
      if (delta == 0) {
        d->seqDup++;
      } else if (delta > 1000) {
        // 设备重启/计数器复位：重新基线，不计为丢包
        d->seqResets++;
        d->seqFirst = f.seq;
      } else if (delta > 1) {
        const uint32_t gap = (uint32_t)delta - 1;
        d->seqMissing += gap;
        d->gapHist[lossBucket(gap)]++;
        if (gap > d->gapMax) d->gapMax = (uint16_t)gap;
      }
    }
    d->seqPrev = f.seq;
    d->seqLast = f.seq;
  }

  // TDMA 时隙定位
  if (slotFrame) {
    if (!d->offSeen) {
      d->offSeen = true;
      d->offMin = d->offMax = offUs;
    } else {
      if (offUs < d->offMin) d->offMin = offUs;
      if (offUs > d->offMax) d->offMax = offUs;
    }
    d->offSum += offUs;
    if (d->offN < 0xFFFF) d->offN++;

    if (!d->respSeen) {
      d->respSeen = true;
      d->respMin = d->respMax = respUs;
    } else {
      if (respUs < d->respMin) d->respMin = respUs;
      if (respUs > d->respMax) d->respMax = respUs;
    }
    d->respSum += respUs;
    if (d->respN < 0xFFFF) d->respN++;
  }
}

// ---------------------------------------------------------------- Ping-Pong
void statsPingSweep(uint32_t nowUs) {
  const uint32_t timeout = (uint32_t)PING_TIMEOUT_MS * 1000u;
  for (uint8_t i = 0; i < 32; i++) {
    if (!s_pingOut[i].used) continue;
    if ((uint32_t)(nowUs - s_pingOut[i].us) > timeout) {
      s_pingOut[i].used = false;
      s_g.ping.lost++;
      s_g.ping.lossRun++;
      if (s_g.ping.lossRun > s_g.ping.lossRunMax) {
        s_g.ping.lossRunMax = s_g.ping.lossRun;
      }
    }
  }
}

void statsOnPingSent(uint16_t seq, uint32_t nowUs) {
  statsPingSweep(nowUs);
  s_g.ping.seen = true;
  s_g.ping.tx++;
  int8_t slot = -1;
  for (uint8_t i = 0; i < 32; i++) {
    if (!s_pingOut[i].used) { slot = (int8_t)i; break; }
  }
  if (slot < 0) {  // 表满：覆盖最旧的一条
    slot = 0;
    for (uint8_t i = 1; i < 32; i++) {
      if (s_pingOut[i].us < s_pingOut[slot].us) slot = (int8_t)i;
    }
  }
  s_pingOut[slot].seq = seq;
  s_pingOut[slot].us = nowUs;
  s_pingOut[slot].used = true;
}

void statsOnPong(uint16_t seq, uint32_t nowUs) {
  for (uint8_t i = 0; i < 32; i++) {
    if (!s_pingOut[i].used || s_pingOut[i].seq != seq) continue;
    const uint32_t rtt = (uint32_t)(nowUs - s_pingOut[i].us);
    s_pingOut[i].used = false;

    s_g.ping.seen = true;
    s_g.ping.rx++;
    s_g.ping.lastPongMs = millis();
    const uint16_t r = (rtt > 0xFFFFu) ? 0xFFFFu : (uint16_t)rtt;
    if (s_g.ping.rx == 1) {
      s_g.ping.rttMin = s_g.ping.rttMax = r;
    } else {
      if (r < s_g.ping.rttMin) s_g.ping.rttMin = r;
      if (r > s_g.ping.rttMax) s_g.ping.rttMax = r;
    }
    s_g.ping.rttSum += rtt;
    if (s_g.ping.n < MAX_RTT_SAMPLES) s_g.ping.samples[s_g.ping.n++] = r;

    // 一段连续丢失结束：记一次分布
    if (s_g.ping.lossRun > 0) {
      s_g.ping.lossHist[lossBucket(s_g.ping.lossRun)]++;
      s_g.ping.lossRun = 0;
    }
    return;
  }
  s_g.ping.unknown++;
}

float statsP95Us() {
  const uint16_t n = s_g.ping.n;
  if (n == 0) return 0.0f;
  memcpy(s_sortBuf, s_g.ping.samples, sizeof(uint16_t) * n);
  // 插入排序：n ≤ 256，够快
  for (uint16_t i = 1; i < n; i++) {
    const uint16_t v = s_sortBuf[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && s_sortBuf[j] > v) {
      s_sortBuf[j + 1] = s_sortBuf[j];
      j--;
    }
    s_sortBuf[j + 1] = v;
  }
  uint16_t idx = (uint16_t)(((uint32_t)n * 95u + 99u) / 100u);
  if (idx == 0) idx = 1;
  if (idx > n) idx = n;
  return (float)s_sortBuf[idx - 1];
}

// ---------------------------------------------------------------- 帧类型名
const char *tdmaTypeName(uint8_t type) {
  switch (type) {
    case TF_BEACON:  return "BEACON";
    case TF_JOIN:    return "JOIN";
    case TF_HIT:     return "HIT";
    case TF_DEATH:   return "DEATH";
    case TF_FIRE:    return "FIRE";
    case TF_HB:      return "HB";
    case TF_CAPTURE: return "CAPTURE";
    case TF_POS:     return "POS";
    case TF_TAG:     return "TAG";
    case TF_LOG:     return "LOG";
    case TF_PAIR:    return "PAIR";
    case TF_WELCOME: return "WELCOME";
    case TF_START:   return "START";
    case TF_END:     return "END";
    case TF_RESPAWN: return "RESPAWN";
    case TF_PAUSE:   return "PAUSE";
    case TF_RESUME:  return "RESUME";
    case TF_XHIT:    return "XHIT";
    case TF_VITAL:   return "VITAL";
    case TF_ASSIGN:  return "ASSIGN";
    case TF_TEST_PING: return "TEST-PING";
    case TF_TEST_PONG: return "TEST-PONG";
    default: return "?";
  }
}
