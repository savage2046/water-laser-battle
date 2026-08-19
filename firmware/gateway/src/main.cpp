#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "RadioLink.h"
#include "TdmaMac.h"
#include "Display.h"

WebSocketsClient ws;
WiFiUDP udp;                     // 网关间组播通道
static bool g_wsConnected = false;
static unsigned long g_lastLedToggle = 0;
static bool g_ledOn = false;

// ===== 射频槽位表（开机自检用）=====
// 每槽 { NSS, BUSY, RST }；SPI 总线共享（SCLK/MOSI/MISO 见 config.h SX_SCLK 等）。
// 0xFF = 槽位未定义/未装（自检跳过）。T1 单射频：只填槽 0（沿用原 PIN_SX_*）。
// T3 多射频板：按实际 PCB 填写全部槽位；没装满时其余槽留 0xFF 即可。
// 注意：用 int16_t（int8_t 存 0xFF 会窄化为 -1，无法与 0xFF 比较）。
static const int16_t kRfSlots[GW_RF_SLOTS][3] = {
    { 5, 17, 16 },  // 槽 0（T1 单射频：NSS=5, BUSY=17, RST=16）
    { 0xFF, 0xFF, 0xFF },  // 槽 1（示例未用；T3 填实际引脚）
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF },
};

// ===== 检测到的射频（每射频 = RadioLink + TdmaMac）=====
struct RfUnit {
  RadioLink link;    // 需要 begin(freq) 后可用（默认构造，槽位引脚在自检时赋）
  TdmaMac mac;       // 每个射频一个 MAC（运行于独立任务）
  uint8_t nss = 0xFF, busy = 0xFF;
  int8_t rst = -1;
  float freqMhz = 0;
  uint8_t gridIdx = 0;   // 标准栅格索引（TF_ASSIGN 告知设备，设备据此跳频）
  bool ok = false;
};
static RfUnit g_rf[GW_RF_SLOTS];
static uint8_t g_rfCount = 0;

// 标准频点栅格（与设备端一致，见 TdmaProto.h TDMA_STD_*）
static float g_stdChannels[TDMA_STD_CHANNELS];

// devIdx → deviceId 映射（J 帧学习，事件帧查表）
struct DevEntry {
  uint8_t idx;
  char devId[24];
  char name[24];
  bool valid;
};
static DevEntry g_devs[24];
static uint8_t g_devCount = 0;

// ===== 一发一杀判定（网关唯一权威，组播协调）=====
struct KillKey {
  uint16_t killer;
  uint8_t shotSeq;
  uint8_t channel;
  uint8_t energy;
  unsigned long ts;
};
static KillKey g_kills[64];
static uint8_t g_killCount = 0;

static int killArbitrate(uint16_t killer, uint8_t shotSeq, uint8_t channel,
                         uint8_t energy) {
  unsigned long now = millis();
  for (uint8_t i = 0; i < 64; i++) {
    if (g_kills[i].killer == killer && g_kills[i].shotSeq == shotSeq) {
      if (now - g_kills[i].ts >= 800) {
        g_kills[i] = { killer, shotSeq, channel, energy, now };
        return 1;
      }
      if (g_kills[i].channel == 0 && channel == 1) return 0;
      if (g_kills[i].channel == 1 && channel == 0) {
        g_kills[i].channel = 0;
        g_kills[i].energy = energy;
        g_kills[i].ts = now;
        return 2;
      }
      if (energy > g_kills[i].energy) {
        g_kills[i].energy = energy;
        g_kills[i].ts = now;
        return 2;
      }
      return 0;
    }
  }
  g_kills[g_killCount % 64] = { killer, shotSeq, channel, energy, now };
  g_killCount++;
  return 1;
}

static const char *devIdOf(uint8_t idx) {
  for (uint8_t i = 0; i < g_devCount; i++)
    if (g_devs[i].valid && g_devs[i].idx == idx) return g_devs[i].devId;
  return NULL;
}

static bool devIdxOf(const char *devId, uint8_t &outIdx) {
  if (!devId) return false;
  for (uint8_t i = 0; i < g_devCount; i++) {
    if (g_devs[i].valid && strcmp(g_devs[i].devId, devId) == 0) {
      outIdx = g_devs[i].idx;
      return true;
    }
  }
  return false;
}

static void learnDev(uint8_t idx, const char *devId, const char *name) {
  for (uint8_t i = 0; i < g_devCount; i++) {
    if (g_devs[i].valid && g_devs[i].idx == idx) {
      strncpy(g_devs[i].devId, devId, sizeof(g_devs[i].devId) - 1);
      strncpy(g_devs[i].name, name, sizeof(g_devs[i].name) - 1);
      return;
    }
  }
  if (g_devCount < 24) {
    g_devs[g_devCount].idx = idx;
    strncpy(g_devs[g_devCount].devId, devId, sizeof(g_devs[g_devCount].devId) - 1);
    strncpy(g_devs[g_devCount].name, name, sizeof(g_devs[g_devCount].name) - 1);
    g_devs[g_devCount].valid = true;
    g_devCount++;
  }
}

// ===== 开机自检：槽位探测 + 信道质量检测 + 频率自动分配 =====

// SPI 读 SX1262 版本寄存器 0x0333（ReadRegister 0x1D）。
// 空槽位 MISO 悬空 → 读回 0x00/0xFF → 判定不存在（快速筛除，再由 begin 确认）。
static bool probeSlot(uint8_t nss, uint8_t busy) {
  pinMode(nss, OUTPUT);
  digitalWrite(nss, HIGH);
  // 等待 BUSY 释放（最多 1ms；空槽位悬空则跳过）
  uint32_t t0 = millis();
  while (digitalRead(busy) == HIGH && millis() - t0 < 1) {}
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(nss, LOW);
  SPI.transfer(0x1D);                 // ReadRegister
  SPI.transfer(0x03);                 // 地址 0x0333 高字节
  SPI.transfer(0x33);                 // 低字节
  uint8_t v0 = SPI.transfer(0x00);    // 版本（首个读字节，按 RadioLib 惯例）
  uint8_t v1 = SPI.transfer(0x00);    // 冗余字节（兼容总线转向）
  digitalWrite(nss, HIGH);
  SPI.endTransaction();
  bool any = (v0 != 0x00 && v0 != 0xFF) || (v1 != 0x00 && v1 != 0xFF);
  if (any) {
    Serial.printf("[self-test] slot NSS=%u version=0x%02X/0x%02X\n", nss, v0,
                  v1);
  }
  return any;
}

// ===== 信道质量检测（避开干扰严重的频率）=====
struct FreqQual {
  float avg;     // RSSI 均值（噪声底）
  int16_t max;   // RSSI 峰值（信号活动）
};
static FreqQual g_freqQual[TDMA_STD_CHANNELS];

// 用射频 0 逐个测量标准栅格各频点信道质量：
// RX 模式采样 GW_FREQ_QUAL_SAMPLES 次 RSSI → 均值/峰值。
// 均值高 = 噪声底偏高；峰值高 = 该频点有信号活动（被占用/干扰）。
static void measureFreqs(SX1262 *r) {
  r->standby();
  for (uint8_t k = 0; k < TDMA_STD_CHANNELS; k++) {
    float f = TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ;
    r->setFrequency(f);   // 需 standby 态（上一点已 standby）
    r->startReceive();
    delay(5);  // RX 稳定
    long sum = 0;
    int16_t mx = -200;
    for (uint16_t i = 0; i < GW_FREQ_QUAL_SAMPLES; i++) {
      int16_t rssi = r->getRSSI();
      if (rssi > mx) mx = rssi;
      sum += rssi;
      delay(GW_FREQ_QUAL_INTERVAL_MS);
    }
    r->standby();
    g_freqQual[k].avg = (float)sum / GW_FREQ_QUAL_SAMPLES;
    g_freqQual[k].max = mx;
    Serial.printf("[self-test] freq %u (%.1fMHz): avg=%.0f max=%d dBm%s\n", k,
                  (double)f, (double)g_freqQual[k].avg, mx,
                  (g_freqQual[k].max > GW_FREQ_MAX_THRESH_DBM ||
                   g_freqQual[k].avg > GW_FREQ_AVG_THRESH_DBM)
                      ? " <-- NOISY"
                      : "");
  }
}

static bool freqNoisy(uint8_t k) {
  return g_freqQual[k].max > GW_FREQ_MAX_THRESH_DBM ||
         g_freqQual[k].avg > GW_FREQ_AVG_THRESH_DBM;
}

// 质量感知分配：先按均匀散布取理想频点（k_i = round(i×19/(M-1))），
// 理想点干扰重则在 ±GW_FREQ_SEARCH_WINDOW 栅格内选质量最优的可用替代点
// （评分 = 质量 + 偏离惩罚）→ 既保持分散又避开干扰；窗口内无干净点则保留并告警。
static void assignFreqs(uint8_t m, float *freqs, uint8_t *gridIdx) {
  if (m == 0) return;
  bool used[TDMA_STD_CHANNELS] = { false };
  for (uint8_t i = 0; i < m; i++) {
    uint8_t k = (m == 1) ? 0
                         : (uint8_t)roundf((float)i * (TDMA_STD_CHANNELS - 1) /
                                           (float)(m - 1));
    if (freqNoisy(k)) {
      uint8_t best = 0xFF;
      float bestScore = 1e9f;
      for (int d = 1; d <= GW_FREQ_SEARCH_WINDOW; d++) {
        for (int s = -1; s <= 1; s += 2) {
          int cand = (int)k + s * d;
          if (cand < 0 || cand >= TDMA_STD_CHANNELS || used[cand]) continue;
          // 评分：质量越好分越低（max 权重 10），偏离理想点越远加分越多
          float sc = (float)(g_freqQual[cand].max - (-120)) * 10.0f +
                     (float)abs(cand - (int)k) * 0.5f;
          if (sc < bestScore) {
            bestScore = sc;
            best = (uint8_t)cand;
          }
        }
      }
      if (best != 0xFF) {
        Serial.printf(
            "[self-test] freq %u (%.1fMHz) noisy (avg=%.0f max=%d), use %u (%.1fMHz)\n",
            k, (double)(TDMA_STD_BASE_MHZ + k * TDMA_STD_STEP_MHZ),
            (double)g_freqQual[k].avg, g_freqQual[k].max, best,
            (double)(TDMA_STD_BASE_MHZ + best * TDMA_STD_STEP_MHZ));
        k = best;
      } else {
        Serial.printf("[self-test] warn: no clean freq near %u, keep it\n", k);
      }
    }
    used[k] = true;
    gridIdx[i] = k;
    freqs[i] = TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ;
  }
}

// ---------- 网关间组播通道 ----------
static void mcastSend(const char *kind, uint8_t idx, uint16_t seq,
                      uint16_t p1 = 0, uint16_t p2 = 0) {
  char buf[100];
  snprintf(buf, sizeof(buf), "WLB1,%s,%s,%u,%u,%u,%u", GATEWAY_ID, kind,
           idx, seq, p1, p2);
  udp.beginPacketMulticast(MCAST_IP, MCAST_PORT, WiFi.localIP());
  udp.print(buf);
  udp.endPacket();
}

static void mcastPoll() {
  int n = udp.parsePacket();
  if (n <= 0) return;
  char buf[100];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  if (strncmp(buf, "WLB1,", 5) != 0) return;
  char srcGw[16], kind[4];
  uint8_t idx = 0;
  uint16_t seq = 0, p1 = 0, p2 = 0;
  int parsed = sscanf(buf + 5, "%15[^,],%3[^,],%hhu,%hu,%hu,%hu",
                      srcGw, kind, &idx, &seq, &p1, &p2);
  if (parsed < 4) return;
  if (strcmp(srcGw, GATEWAY_ID) == 0) return;

  Serial.printf("[mesh] %s -> %s: %s devIdx=%u seq=%u", srcGw, GATEWAY_ID,
                kind, idx, seq);
  if (kind[0] == 'H') Serial.printf(" shooter=%u hp=%u", p1, p2);
  if (kind[0] == 'D') Serial.printf(" killer=%u", p1);
  if (kind[0] == 'K') {
    Serial.printf(" kill-sync killer=%u seq=%u ch=%u energy=%u", idx, seq, p2,
                  p1);
    killArbitrate(idx, (uint8_t)seq, (uint8_t)p2, (uint8_t)p1);
  }
  Serial.println();
  if (kind[0] != 'K') {
    gwDisplay.showEvent(kind, idx, seq, p1, p2);
    digitalWrite(PIN_STATUS_LED, LOW);
  }
}

// ---------- TDMA 上行二进制帧 → JSON（dev*）----------
static void sendDev(const char *t, uint8_t idx, uint16_t seq) {
  const char *devId = devIdOf(idx);
  if (!devId) {
    Serial.printf("[up] unknown devIdx=%u, drop %s\n", idx, t);
    return;
  }
  JsonDocument doc;
  doc["t"] = t;
  doc["gatewayId"] = GATEWAY_ID;
  doc["deviceId"] = devId;
  doc["seq"] = seq;
  String out;
  serializeJson(doc, out);
  ws.sendTXT(out);
}

// P 帧 3 片重组（按 devIdx）
struct PosBuf {
  bool got[3];
  long lat, lon;
  int16_t yaw;
  int8_t pitch, roll;
};
static PosBuf g_pos[24];
static void posFrag(const TdmaFrame &f) {
  uint8_t idx = f.devIdx;
  if (idx > 23) return;
  PosBuf &pb = g_pos[idx];
  if (f.flags & TF_FLAG_FRAG_FIRST) {
    pb.got[0] = pb.got[1] = pb.got[2] = false;
    pb.lat = (long)(((uint32_t)f.payload[0] << 24) |
                    ((uint32_t)f.payload[1] << 16) |
                    ((uint32_t)f.payload[2] << 8) | f.payload[3]);
    pb.got[0] = true;
  } else if (f.flags & TF_FLAG_FRAG_MORE) {
    pb.lon = (long)(((uint32_t)f.payload[0] << 24) |
                    ((uint32_t)f.payload[1] << 16) |
                    ((uint32_t)f.payload[2] << 8) | f.payload[3]);
    pb.got[1] = true;
  } else {
    pb.yaw = (int16_t)((f.payload[0] << 8) | f.payload[1]);
    pb.pitch = (int8_t)f.payload[2];
    pb.roll = (int8_t)f.payload[3];
    pb.got[2] = true;
  }
  if (pb.got[0] && pb.got[1] && pb.got[2]) {
    pb.got[0] = pb.got[1] = pb.got[2] = false;
    const char *devId = devIdOf(idx);
    if (!devId) return;
    JsonDocument doc;
    doc["t"] = "devPos";
    doc["gatewayId"] = GATEWAY_ID;
    doc["deviceId"] = devId;
    doc["seq"] = f.seq;
    doc["lat"] = pb.lat / 1000000.0;
    doc["lon"] = pb.lon / 1000000.0;
    doc["yaw"] = pb.yaw;
    doc["pitch"] = pb.pitch;
    doc["roll"] = pb.roll;
    String out;
    serializeJson(doc, out);
    ws.sendTXT(out);
  }
}

static void onTdmaUplink(uint8_t rfIdx, const TdmaFrame &f) {
  switch (f.type) {
    case TF_JOIN: {  // payload = 5B deviceId（头盔带 TF_FLAG_HELMET）
      char devId[6];
      for (int i = 0; i < 5; i++)
        devId[i] = f.payload[i] ? (char)f.payload[i] : ' ';
      devId[5] = '\0';
      for (int i = 4; i >= 0; i--) {
        if (devId[i] == ' ') devId[i] = '\0';
        else break;
      }
      learnDev(f.devIdx, devId, devId);
      JsonDocument doc;
      doc["t"] = "devHello";
      doc["gatewayId"] = GATEWAY_ID;
      doc["deviceId"] = devId;
      doc["name"] = devId;
      doc["fw"] = FW_VERSION;
      if (f.flags & TF_FLAG_HELMET) doc["helmet"] = true;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      Serial.printf("[up] ch%u devHello idx=%u dev=%s%s\n", rfIdx, f.devIdx,
                    devId,
                    (f.flags & TF_FLAG_HELMET) ? " (helmet)" : "");
      break;
    }
    case TF_HIT: {
      uint16_t shooter = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
      uint8_t weapon = (uint8_t)(f.payload[3] >> 4);
      uint8_t channel = (uint8_t)(f.payload[3] & 0x0F);
      uint8_t shotSeq = f.payload[2];
      uint8_t hp = f.payload[4];
      mcastSend("H", f.devIdx, f.seq, shooter, hp);
      gwDisplay.showEvent("H", f.devIdx, f.seq, shooter, hp);
      const char *devId = devIdOf(f.devIdx);
      if (!devId) break;
      JsonDocument doc;
      doc["t"] = "devHit";
      doc["gatewayId"] = GATEWAY_ID;
      doc["deviceId"] = devId;
      doc["seq"] = f.seq;
      doc["shooter"] = shooter;
      doc["weapon"] = weapon;
      doc["shotSeq"] = shotSeq;
      doc["channel"] = channel;
      doc["hp"] = hp;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      break;
    }
    case TF_DEATH: {
      uint16_t killer = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
      uint8_t shotSeq = f.payload[2];
      uint8_t channel = f.payload[3];
      uint8_t energy = f.payload[4];
      mcastSend("D", f.devIdx, f.seq, killer, shotSeq);
      gwDisplay.showEvent("D", f.devIdx, f.seq, killer, shotSeq);
      const char *devId = devIdOf(f.devIdx);
      if (!devId) break;
      int dedup = killArbitrate(killer, shotSeq, channel, energy);
      bool newKill = (dedup == 1 || dedup == 2);
      bool upgrade = (dedup == 2);
      if (dedup != 0) {
        mcastSend("K", killer, shotSeq, energy, channel);
      }
      JsonDocument doc;
      doc["t"] = "devDeath";
      doc["gatewayId"] = GATEWAY_ID;
      doc["deviceId"] = devId;
      doc["seq"] = f.seq;
      doc["killer"] = killer;
      doc["shotSeq"] = shotSeq;
      doc["channel"] = channel;
      doc["energy"] = energy;
      doc["newKill"] = newKill;
      doc["upgrade"] = upgrade;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      break;
    }
    case TF_FIRE:
      sendDev("devFire", f.devIdx, f.seq);
      break;
    case TF_HB:
      sendDev("devHb", f.devIdx, f.seq);
      break;
    case TF_CAPTURE: {
      mcastSend("C", f.devIdx, f.seq);
      gwDisplay.showEvent("C", f.devIdx, f.seq, 0, 0);
      const char *devId = devIdOf(f.devIdx);
      if (!devId) break;
      JsonDocument doc;
      doc["t"] = "devCapture";
      doc["gatewayId"] = GATEWAY_ID;
      doc["deviceId"] = devId;
      doc["seq"] = f.seq;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      break;
    }
    case TF_POS:
      posFrag(f);
      break;
    case TF_TAG: {
      uint16_t shooter = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
      uint8_t weapon = f.payload[2];
      uint8_t shotSeq = f.payload[3];
      uint8_t channel = f.payload[4];
      mcastSend("T", f.devIdx, f.seq, shooter, weapon);
      gwDisplay.showEvent("T", f.devIdx, f.seq, shooter, weapon);
      const char *devId = devIdOf(f.devIdx);
      if (!devId) break;
      JsonDocument doc;
      doc["t"] = "devTag";
      doc["gatewayId"] = GATEWAY_ID;
      doc["deviceId"] = devId;
      doc["seq"] = f.seq;
      doc["shooter"] = shooter;
      doc["weapon"] = weapon;
      doc["shotSeq"] = shotSeq;
      doc["channel"] = channel;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      break;
    }
    case TF_LOG: {
      const char *devId = devIdOf(f.devIdx);
      if (!devId) break;
      char payload[4] = { (char)f.payload[2], (char)f.payload[3],
                          (char)f.payload[4], '\0' };
      JsonDocument doc;
      doc["t"] = "devLog";
      doc["gatewayId"] = GATEWAY_ID;
      doc["deviceId"] = devId;
      doc["seq"] = f.seq;
      doc["part"] = f.payload[0];
      doc["total"] = f.payload[1];
      doc["payload"] = payload;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      break;
    }
    case TF_PAIR: {
      const char *devId = devIdOf(f.devIdx);
      if (!devId) break;
      JsonDocument doc;
      doc["t"] = "devPair";
      doc["gatewayId"] = GATEWAY_ID;
      doc["deviceId"] = devId;
      doc["seq"] = f.seq;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      break;
    }
    default:
      Serial.printf("[up] ch%u unknown type %u\n", rfIdx, f.type);
      break;
  }
}

// ---------- 服务器 JSON → TDMA 下行二进制帧 ----------
static uint8_t g_lastBcType = 0xFF;
static uint32_t g_lastBcAt = 0;
static bool bcCoalesce(uint8_t type) {
  if (g_lastBcType == type && millis() - g_lastBcAt < 2000) return false;
  g_lastBcType = type;
  g_lastBcAt = millis();
  return true;
}

// 广播到所有检测到的射频（每个射频的广播窗各发 1 帧）
static void broadcastAll(uint8_t type, const uint8_t *payload5, uint8_t flags) {
  for (uint8_t j = 0; j < g_rfCount; j++) {
    g_rf[j].mac.sendDownlink(type, payload5, flags, TF_BROADCAST_IDX, 0);
  }
}

static void onServerCmd(const char *devId, JsonObject msg) {
  uint8_t targetIdx = 0;
  if (!devIdxOf(devId, targetIdx)) {
    Serial.printf("[down] unknown target devId=%s, drop\n", devId ? devId : "?");
    return;
  }
  const char *t = msg["t"] | "";
  if (strcmp(t, "welcome") == 0) {
    JsonObject cfg = msg["cfg"];
    uint16_t pid = (uint16_t)(msg["playerId"] | 0);
    uint8_t hp = (uint8_t)(cfg["hp"] | 100);
    uint8_t dmg = (uint8_t)(cfg["dmg"] | 10);
    uint8_t ammo = (uint8_t)(cfg["ammo"] | 120);
    uint16_t reloadMs = (uint16_t)(cfg["reloadMs"] | 2000);
    uint16_t respawnMs = (uint16_t)(cfg["respawnMs"] | 3000);
    int8_t team = (int8_t)(cfg["team"] | 0);
    uint8_t scoreToWin = (uint8_t)(cfg["scoreToWin"] | 50);
    uint8_t powerLevel = (uint8_t)(cfg["powerLevel"] | 1);
    uint32_t fb = 0;
    if (cfg["friends"].is<JsonArray>()) {
      for (JsonVariant v : cfg["friends"].as<JsonArray>()) {
        int fid = v.as<int>();
        if (fid >= 0 && fid < 24) fb |= (1u << fid);
      }
    }
    uint8_t p0[5] = { (uint8_t)(pid >> 8), (uint8_t)pid, hp, dmg, ammo };
    uint8_t p1[5] = { (uint8_t)(reloadMs >> 8), (uint8_t)reloadMs,
                      (uint8_t)(respawnMs >> 8), (uint8_t)respawnMs,
                      (uint8_t)team };
    uint8_t p2[5] = { scoreToWin, powerLevel, (uint8_t)(fb >> 16),
                      (uint8_t)(fb >> 8), (uint8_t)fb };
    // 设备可能注册在任意射频（信道），向全部射频各发一份 welcome 分片
    for (uint8_t j = 0; j < g_rfCount; j++) {
      g_rf[j].mac.sendDownlink(TF_WELCOME, p0,
                               TF_FLAG_FRAG_FIRST | TF_FLAG_FRAG_MORE,
                               targetIdx, 0);
      g_rf[j].mac.sendDownlink(TF_WELCOME, p1, TF_FLAG_FRAG_MORE, targetIdx, 0);
      g_rf[j].mac.sendDownlink(TF_WELCOME, p2, 0, targetIdx, 0);
    }
    Serial.printf("[down] W welcome to all ch (target %u)\n", targetIdx);
  } else if (strcmp(t, "match") == 0) {
    const char *status = msg["status"] | "";
    if (strcmp(status, "start") == 0) {
      uint8_t p[5] = { 0, 0, 0, 0, 0 };
      if (bcCoalesce(TF_START)) {
        broadcastAll(TF_START, p, 0);
        Serial.printf("[down] S broadcast\n");
      }
    } else if (strcmp(status, "end") == 0) {
      int winner = msg["winner"] | -1;
      uint8_t p[5] = { (uint8_t)(winner < 0 ? 0xFF : winner), 0, 0, 0, 0 };
      if (bcCoalesce(TF_END)) {
        broadcastAll(TF_END, p, 0);
        Serial.printf("[down] E broadcast winner=%d\n", winner);
      }
    }
  } else if (strcmp(t, "control") == 0) {
    const char *action = msg["action"] | "";
    if (strcmp(action, "respawn") == 0) {
      // 设备注册在哪个射频未知 → 向全部射频发（设备按 devIdx 过滤）
      for (uint8_t j = 0; j < g_rfCount; j++) {
        g_rf[j].mac.sendDownlink(TF_RESPAWN, NULL, 0, targetIdx, 0);
      }
      Serial.printf("[down] R respawn to all ch (target %u)\n", targetIdx);
    } else if (strcmp(action, "pause") == 0) {
      if (bcCoalesce(TF_PAUSE)) broadcastAll(TF_PAUSE, NULL, 0);
    } else if (strcmp(action, "resume") == 0) {
      if (bcCoalesce(TF_RESUME)) broadcastAll(TF_RESUME, NULL, 0);
    } else if (strcmp(action, "hit") == 0) {
      uint16_t shooter = (uint16_t)(msg["shooter"] | 0);
      uint8_t p[5] = { (uint8_t)(shooter >> 8), (uint8_t)shooter,
                       (uint8_t)(msg["weapon"] | 0),
                       (uint8_t)(msg["shotSeq"] | 0),
                       (uint8_t)(msg["channel"] | 0) };
      for (uint8_t j = 0; j < g_rfCount; j++) {
        g_rf[j].mac.sendDownlink(TF_XHIT, p, 0, targetIdx, 0);
      }
      Serial.printf("[down] X hit to all ch (target %u)\n", targetIdx);
    } else if (strcmp(action, "vital") == 0) {
      uint16_t pid = (uint16_t)(msg["playerId"] | 0);
      uint8_t p[5] = { (uint8_t)(pid >> 8), (uint8_t)pid,
                       (uint8_t)(msg["hp"] | 100),
                       (uint8_t)(msg["alive"] | 1), 0 };
      for (uint8_t j = 0; j < g_rfCount; j++) {
        g_rf[j].mac.sendDownlink(TF_VITAL, p, 0, targetIdx, 0);
      }
    }
  }
}

void wsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[ws] connected\n");
      g_wsConnected = true;
      JsonDocument doc;
      doc["t"] = "gatewayHello";
      doc["gatewayId"] = GATEWAY_ID;
      doc["name"] = GATEWAY_NAME;
      doc["fw"] = FW_VERSION;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[ws] disconnected\n");
      g_wsConnected = false;
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) {
        Serial.printf("[ws] bad json: %s\n", err.c_str());
        break;
      }
      const char *t = doc["t"] | "";
      if (strcmp(t, "cmd") == 0 && doc["msg"].is<JsonObject>()) {
        onServerCmd(doc["deviceId"] | "", doc["msg"].as<JsonObject>());
      }
      break;
    }
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_STATUS_LED, OUTPUT);

  // ===== 开机自检：标准栅格 + 槽位探测 + 频率自动分配 =====
  SPI.begin(SX_SCLK, SX_MISO, SX_MOSI, SX_NSS);
  for (int k = 0; k < TDMA_STD_CHANNELS; k++) {
    g_stdChannels[k] = TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ;
  }

  // 1) 探测每个槽位（版本寄存器快速筛除空槽）
  uint8_t present[GW_RF_SLOTS];
  uint8_t m = 0;
  for (uint8_t i = 0; i < GW_RF_SLOTS; i++) {
    if (kRfSlots[i][0] == 0xFF) continue;  // 未定义/未装槽位
    if (probeSlot((uint8_t)kRfSlots[i][0], (uint8_t)kRfSlots[i][1])) {
      present[m++] = i;
    }
  }
  Serial.printf("[self-test] %u/%u SX1262 slots present\n", m, GW_RF_SLOTS);

  // 2) 先以临时频点（470.0）初始化全部检测到的射频（供信道质量测量）
  g_rfCount = 0;
  for (uint8_t j = 0; j < m; j++) {
    uint8_t i = present[j];
    RfUnit &u = g_rf[g_rfCount];
    u.nss = (uint8_t)kRfSlots[i][0];
    u.busy = (uint8_t)kRfSlots[i][1];
    u.rst = kRfSlots[i][2];
    u.freqMhz = TDMA_STD_BASE_MHZ;  // 临时频点，测量后重新分配
    u.gridIdx = 0;
    u.link = RadioLink(u.nss, u.rst, u.busy, -1);  // DIO1 不接（轮询模式）
    if (!u.link.begin(u.freqMhz)) continue;
    u.ok = true;
    g_rfCount++;
  }
  if (g_rfCount == 0) {
    Serial.println("[self-test] FATAL: no radio up, abort");
    while (1) { delay(1000); }
  }

  // 3) 信道质量检测：用射频 0 逐个测量标准栅格频点（避开干扰严重的频率）
  measureFreqs(g_rf[0].link.getRadio());

  // 4) 质量感知频率分配：均匀散布基线 + 局部替换避开干扰频点
  float freqs[GW_RF_SLOTS];
  uint8_t gridIdx[GW_RF_SLOTS];
  assignFreqs(g_rfCount, freqs, gridIdx);

  // 5) 应用最终频率 + 启动各射频 MAC（TF_ASSIGN 携带栅格索引，设备据此跳频）
  for (uint8_t j = 0; j < g_rfCount; j++) {
    RfUnit &u = g_rf[j];
    u.freqMhz = freqs[j];
    u.gridIdx = gridIdx[j];
    u.link.getRadio()->standby();          // setFrequency 需 standby 态
    u.link.getRadio()->setFrequency(u.freqMhz);
    u.mac.begin(TdmaMac::ROLE_GATEWAY, TF_BROADCAST_IDX, u.link.getRadio(),
                g_stdChannels, TDMA_STD_CHANNELS, u.gridIdx, TDMA_MAX_SLOTS);
    if (!u.mac.started()) {
      Serial.printf("[self-test] ch%u MAC task fail, skip\n", j);
      u.ok = false;
      continue;
    }
    Serial.printf("[self-test] ch%u (%.1fMHz, grid k=%u) up\n", j,
                  (double)u.freqMhz, u.gridIdx);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[wifi] connecting %s\n", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());

  ws.begin(SERVER_HOST, SERVER_PORT, WS_PATH);
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(3000);

  udp.beginMulticast(MCAST_IP, MCAST_PORT);
  gwDisplay.begin(PIN_OLED_SDA, PIN_OLED_SCL, OLED_ADDR);
  Serial.printf("[gw] %u radios ready, gateway up\n", g_rfCount);
}

void loop() {
  ws.loop();

  mcastPoll();

  // 每个射频的 TDMA 上行 → 服务器（各 MAC 任务共听时隙）
  for (uint8_t j = 0; j < g_rfCount; j++) {
    TdmaFrame f;
    while (g_rf[j].mac.pollUplink(f)) {
      onTdmaUplink(j, f);
    }
  }

  // 状态 LED：WS 在线常亮，离线闪烁
  if (millis() - g_lastLedToggle > (g_wsConnected ? 2000 : 300)) {
    g_lastLedToggle = millis();
    g_ledOn = g_wsConnected ? true : !g_ledOn;
    digitalWrite(PIN_STATUS_LED, g_ledOn);
  }

  // 显示屏：连接状态 + 全信道设备总数
  uint8_t total = 0;
  for (uint8_t j = 0; j < g_rfCount; j++) total += g_rf[j].mac.activeCount();
  gwDisplay.update(g_wsConnected, total, WiFi.localIP().toString().c_str());
}
