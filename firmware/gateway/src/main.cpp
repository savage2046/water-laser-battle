#include <Arduino.h>
#include "config.h"    // ⚠️ 必须放在最前：下面的 #if GW_WIFI_ENABLE 要用到它
// WiFi/服务器相关头文件：GW_WIFI_ENABLE=0 时整体不编译（先跑通 LoRa）
#if GW_WIFI_ENABLE
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebSocketsClient.h>
#endif
#include <ArduinoJson.h>
#include "RadioLink.h"
#include "TdmaMac.h"
#include "Display.h"

#if GW_WIFI_ENABLE
WebSocketsClient ws;
WiFiUDP udp;                     // 网关间组播通道
#endif
static bool g_wsConnected = false;   // WiFi 关闭时恒 false（LED 闪烁 / 状态行 ws=down）
static unsigned long g_lastLedToggle = 0;
static bool g_ledOn = false;

// 状态灯：**G48 模组板载 LED，普通单色灯、灌电流接法 = 低电平点亮**（与枪端一致）。
// ⚠️ 不要直接 digitalWrite(PIN_LED, HIGH/LOW) —— 极性是反的，写 HIGH 是"灭"。
static inline void statusLedWrite(bool on) {
  digitalWrite(PIN_LED, on ? LED_ON_LEVEL : (LED_ON_LEVEL == LOW ? HIGH : LOW));
}

// 上行发送包装：WiFi 关闭时是空操作（或镜像到串口）——
// 这样各处业务上报（sendDev 与各事件）一行都不用改，也不用到处写 #if。
// 形参用 String：原先各调用点传的是 serializeJson(doc, out) 产出的 String，
// ws.sendTXT() 本身就接受 String（写 const char* 会编译报 cannot convert）。
static inline void wsSend(const String &s) {
#if GW_WIFI_ENABLE
  ws.sendTXT(s);
#else
#if GW_MIRROR_UPLINK_SERIAL
  Serial.printf("[up-json] %s\n", s.c_str());   // 本该发服务器的内容 → 串口，用于确认 LoRa 通了
#else
  (void)s;
#endif
#endif
}

// 状态行/屏幕要显示本机 IP：WiFi 关闭时给占位符，省得到处 #if
static inline const char *localIpStr() {
#if GW_WIFI_ENABLE
  static char ip[16];
  IPAddress a = WiFi.localIP();
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  return ip;
#else
  return "-";
#endif
}

// ===== 射频槽位表（开机自检用）=====
// 每槽 { NSS, BUSY, RST }；SPI 总线共享（SCLK/MOSI/MISO 见 config.h PIN_SX_*）。
// 每槽 { NSS, BUSY, RST, DIO1 }；SPI 总线共享（SCLK/MOSI/MISO 见 config.h PIN_SX_*）。
// ⚠️ DIO1 是**每射频独享**的中断脚，多射频板没有这个引脚预算（N 个射频要 N 根线）
//    → 除了第一个/唯一一个射频用 PIN_SX_DIO1（枪端/首板 = G04），其余一律填 -1。
//    TdmaMac 的收发已改为**轮询 IRQ 寄存器**（不依赖 DIO1，见
//    docs/lora-gateway-test.md §4.1），所以 -1 也不影响功能；接上的那一路可以用来
//    跑 RadioLib 阻塞式 API（transmit/receive）或做精确到达时刻。
// 0xFF = 槽位未定义/未装（自检跳过）。T1 单射频：只填槽 0（沿用原 PIN_SX_*）。
// T3 多射频板：按实际 PCB 填写全部槽位；没装满时其余槽留 0xFF 即可。
// 注意：用 int16_t（int8_t 存 0xFF 会窄化为 -1，无法与 0xFF 比较）。
static const int16_t kRfSlots[GW_RF_SLOTS][4] = {
    // 槽 0（已实现）：引脚直接引用 config.h —— 2026-09-12 起网关与枪端接线相同
    { PIN_SX_NSS, PIN_SX_BUSY, PIN_SX_RST, PIN_SX_DIO1 },
    // 槽 1..3（**目标 3 信道 = 50 台**，2026-09-13 定案）：待多射频板 PCB 定稿后填实际引脚。
    //   每槽只需 NSS + BUSY 两根（RST 可多模块共用一根；DIO1 一律填 -1，收发改轮询 IRQ）。
    //   填好后开机自检会自动认出并按噪声质量分配频点（assignFreqs：3 信道间隔约 18~20MHz）。
    { 0xFF, 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0xFF, 0xFF },
};

// ===== 检测到的射频（每射频 = RadioLink + TdmaMac）=====
struct RfUnit {
  RadioLink link;    // 需要 begin(freq) 后可用（默认构造，槽位引脚在自检时赋）
  TdmaMac mac;       // 每个射频一个 MAC（运行于独立任务）
  uint8_t nss = 0xFF, busy = 0xFF;
  int8_t rst = -1;
  int8_t dio1 = -1;          // SX1268 DIO1（槽 0 = PIN_SX_DIO1/G04；其余 -1）
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

// 空槽位快速筛除：SPI 读 SX1268 版本串 0x0320（ReadRegister 0x1D）。
//
// ⚠️ 2026-09-13 修正（关键）：探针必须**自己驱动 RST 并做复位脉冲**，不能指望 begin()。
//    本函数跑在 RadioLink::begin() 之前，而 RadioLib 的 SX126x::reset()（RST 输出低 1ms
//    → 高 10ms，SX126x.cpp）是**全工程唯一会驱动 RST 的地方** —— 探针这一刻 G46 还停在
//    上电默认态。G46 是 ESP32-S3 的 strapping 脚：软件不配置时电平由**芯片内部弱上拉/
//    弱下拉**决定（ESP32-S3-WROOM-1 数据手册 §3.3 Strapping Pins：接高阻或不接时，
//    strapping 脚的默认输入电平由内部弱上拉/下拉决定）。此时 NRESET 由 SX1268 内部
//    ~50k 上拉（数据手册表 8-3 "IN PU"）与 S3 内部弱下拉**分压 ≈1.6V**，正好落在
//    NRESET 阈值不确定区 → 芯片可能一直停在复位态、SPI 完全不应答（MISO 读回 0x00/0xFF）
//    → 返回 false → m=0 → 开机误报 FATAL "no radio up"，把矛头错指向"焊接/供电"。
//    对照：昨晚 spi-read 能读到版本串，正因为它显式
//    pinMode(RST,OUTPUT) + digitalWrite(RST,HIGH)，并做了 2ms 复位脉冲
//    （spi-read/src/main.cpp:57-58、315-317）。
//
// 字节布局（昨晚实测，与 RadioLib Module.cpp:402 `memcpy(dataIn,&buffIn[cmdLen+1],…)`
// 一致）：3 字节命令之后 rx[3] = **状态字节**（活芯片非 0x00/0xFF，实测 0xA2），
// rx[4..] = 数据。旧代码把 rx[3] 当"版本首字节"是错的（这正是 spi-read 踩过的偏移坑）。
static const uint16_t kSlotRstLowMs = 2;   // 复位脉冲低电平时长（SX126x 要求 >100µs）

static bool probeSlot(uint8_t nss, uint8_t rst, uint8_t busy) {
  pinMode(nss, OUTPUT);
  digitalWrite(nss, HIGH);   // NSS 低有效：空闲与复位期间必须为高
  pinMode(busy, INPUT);      // 模组驱动，只读

  // ① 驱动前先读 RST 引脚的原始电平：读到 0 = NRESET 本来就被压住（芯片一直在复位）
  //    ⚠️ 这里**故意不先 pinMode(rst, INPUT)**：ESP32 Arduino 的 pinMode(INPUT) 可能把
  //    上电时的内部弱下拉/上拉一起清掉，那就把要测的电平抹掉了。直接 digitalRead 读的
  //    是上电默认态下的真实电平。注意这只是**指示性**读数（阈值≈1.65V，分压点附近会抖）；
  //    权威判据是用万用表量模组 RES 脚对 GND 的静态电压（见 FATAL 提示）。
  int rstBefore = -1;
  if (rst != 0xFF) {
    rstBefore = digitalRead(rst);
    pinMode(rst, OUTPUT);
    digitalWrite(rst, HIGH);      // RST 低有效：强制解除复位，消除分压不确定态
  }
  // ② 标准复位脉冲（SX126x 要求 >100µs；2ms 富余，与 spi-read 一致）
  if (rst != 0xFF) {
    digitalWrite(rst, LOW);
    delay(kSlotRstLowMs);
    digitalWrite(rst, HIGH);
    delay(10);                    // 复位后等芯片就绪
  }
  // ③ 等 BUSY 释放（最多 20ms；空槽位 BUSY 悬空会立即通过）
  uint32_t t0 = millis();
  while (digitalRead(busy) == HIGH && millis() - t0 < 20) {}

  // ④ 读版本串 0x0320 共 16 字节（与 RadioLink::selfCheck、RadioLib findChip 同一寄存器）
  uint8_t ver[16] = {0};
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(nss, LOW);
  SPI.transfer(0x1D);                      // ReadRegister
  SPI.transfer(0x03);                      // 地址 0x0320 高字节
  SPI.transfer(0x20);                      // 低字节
  const uint8_t st = SPI.transfer(0x00);   // rx[3] = 状态字节
  for (uint8_t k = 0; k < 16; k++) ver[k] = SPI.transfer(0x00);  // rx[4..] = 数据
  digitalWrite(nss, HIGH);
  SPI.endTransaction();

  char asc[17];
  for (uint8_t k = 0; k < 16; k++)
    asc[k] = (ver[k] >= 0x20 && ver[k] <= 0x7E) ? (char)ver[k] : '.';
  asc[16] = '\0';
  const char *want = "SX126";               // SX1268 的版本串以 "SX126" 开头
  bool verOk = true;
  for (uint8_t k = 0; k < 5; k++)
    if (asc[k] != want[k]) verOk = false;
  const bool any = verOk || (st != 0x00 && st != 0xFF);

  Serial.printf("[self-test] slot nss=G%u rst=G%u(before=%d) st=0x%02X ver=\"%s\""
                " -> %s\n",
                (unsigned)nss, (unsigned)rst, rstBefore, (unsigned)st, asc,
                any ? "present" : "absent");
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
static void measureFreqs(SX126x *r) {
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

// ---------- 网关间组播通道（GW_WIFI_ENABLE=0 时整段不编译）----------
#if GW_WIFI_ENABLE
static void mcastSend(const char *kind, uint8_t idx, uint16_t seq,
                      uint16_t p1 = 0, uint16_t p2 = 0) {
  char buf[100];
  snprintf(buf, sizeof(buf), "WLB1,%s,%s,%u,%u,%u,%u", GATEWAY_ID, kind,
           idx, seq, p1, p2);
  // ESP32 core 2.0.x 的 WiFiUDP 没有 beginPacketMulticast：
  //   接收组播用 beginMulticast()（setup 里已调用）
  //   发送组播直接用 beginPacket(组播地址, 端口)（TTL 默认 1，局限在本网段）
  udp.beginPacket(MCAST_IP, MCAST_PORT);
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
    statusLedWrite(true);   // 事件闪一下（G48 单色灯，低电平点亮）
  }
}
#else
// WiFi 关闭：组播收发都是空操作（调用点一行都不用改）
static inline void mcastSend(const char *, uint8_t, uint16_t, uint16_t = 0,
                             uint16_t = 0) {}
static inline void mcastPoll() {}
#endif

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
  wsSend(out);
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
    wsSend(out);
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
      wsSend(out);
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
      wsSend(out);
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
      wsSend(out);
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
      wsSend(out);
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
      wsSend(out);
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
      wsSend(out);
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
      wsSend(out);
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

#if GW_WIFI_ENABLE
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
      wsSend(out);
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
#endif  // GW_WIFI_ENABLE（wsEvent：服务器下发命令的处理）

// ===== S3 引脚合法性检查（换板后最容易踩的坑，开机就报）=====
// 返回 true = 该脚在 ESP32-S3 上不可用；why 给出原因。strapping 脚单独给"注意"。
static bool pinBadS3(int pin, const char **why) {
  if (pin < 0) return false;
  if (pin == 19 || pin == 20) { *why = "G19/G20 是 ESP32-S3 的 USB D-/D+"; return true; }
  if (pin >= 26 && pin <= 32) { *why = "G26~G32 接 SPI Flash"; return true; }
  if (pin >= 33 && pin <= 37) { *why = "G33~G37 接 Octal PSRAM（R8 模组）"; return true; }
  return false;
}
static bool pinStrapS3(int pin) {
  return (pin == 0 || pin == 3 || pin == 45 || pin == 46);
}

// 检查 SPI 总线 4 根 + 每个已定义槽位的 NSS/BUSY/RST/DIO1
static void checkPinsForS3() {
  struct { const char *name; int pin; } bus[] = {
      {"NSS", PIN_SX_NSS},   {"SCLK", PIN_SX_SCLK},
      {"MOSI", PIN_SX_MOSI}, {"MISO", PIN_SX_MISO}};
  int bad = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const char *why = nullptr;
    if (pinBadS3(bus[i].pin, &why)) {
      Serial.printf("[self-test] ❌ 引脚冲突：%s=G%d —— %s\n", bus[i].name,
                    bus[i].pin, why);
      bad++;
    } else if (pinStrapS3(bus[i].pin)) {
      Serial.printf("[self-test] ⚠ %s=G%d 是 S3 strapping 脚（上电瞬间被采样）\n",
                    bus[i].name, bus[i].pin);
    }
  }
  for (uint8_t i = 0; i < GW_RF_SLOTS; i++) {
    if (kRfSlots[i][0] == 0xFF) continue;
    for (uint8_t k = 0; k < 4; k++) {
      const char *why = nullptr;
      if (pinBadS3((int)kRfSlots[i][k], &why)) {
        Serial.printf("[self-test] ❌ 槽 %u 第 %u 个脚 G%d 冲突 —— %s\n", i, k,
                      (int)kRfSlots[i][k], why);
        bad++;
      }
    }
  }
  if (bad == 0) {
    Serial.printf("[self-test] pins ok (S3): NSS=G%d SCLK=G%d MOSI=G%d MISO=G%d"
                  " RST=G%d BUSY=G%d DIO1=G%d\n",
                  PIN_SX_NSS, PIN_SX_SCLK, PIN_SX_MOSI, PIN_SX_MISO, PIN_SX_RST,
                  PIN_SX_BUSY, PIN_SX_DIO1);
  }
}

void setup() {
  Serial.begin(115200);
  // ⚠️ ESP32-S3 + ARDUINO_USB_CDC_ON_BOOT=1：**主机（串口监视器）接入之前的所有打印
  //    都会被丢弃** —— 表现就是"刷完固件打开监视器什么都没有"。
  //    所以：① 开机最多等主机 2.5s  ② loop 里主机接入时补打报告  ③ 每 5s 一行心跳
  for (uint32_t t = millis(); !Serial && (millis() - t) < 2500;) delay(20);
  pinMode(PIN_LED, OUTPUT);
  statusLedWrite(false);   // 开机先灭（G48 低电平点亮，别写 LOW）

  // ===== 开机自检：引脚合法性 + 标准栅格 + 槽位探测 + 频率自动分配 =====
  checkPinsForS3();
  SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);
  for (int k = 0; k < TDMA_STD_CHANNELS; k++) {
    g_stdChannels[k] = TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ;
  }

  // 1) 探测每个槽位（版本寄存器快速筛除空槽）
  uint8_t present[GW_RF_SLOTS];
  uint8_t m = 0;
  for (uint8_t i = 0; i < GW_RF_SLOTS; i++) {
    if (kRfSlots[i][0] == 0xFF) continue;  // 未定义/未装槽位
    // 参数顺序：NSS、RST、BUSY（RST 现在必须传进去——探针自己要驱动它并复位）
    if (probeSlot((uint8_t)kRfSlots[i][0], (uint8_t)kRfSlots[i][2],
                  (uint8_t)kRfSlots[i][1])) {
      present[m++] = i;
    }
  }
  Serial.printf("[self-test] %u/%u SX1262 slots present\n", m, GW_RF_SLOTS);

  // DIO1（中断线）逐槽报告：接了的槽位 != -1，便于上板确认 G04 有没有生效
  for (uint8_t i = 0; i < GW_RF_SLOTS; i++) {
    if (kRfSlots[i][0] == 0xFF) continue;
    if (kRfSlots[i][3] >= 0) {
      Serial.printf("[self-test] slot %u DIO1 = GPIO%d（已接线）\n", i,
                    (int)kRfSlots[i][3]);
    } else {
      Serial.printf("[self-test] slot %u DIO1 未接（收发走轮询 IRQ 寄存器）\n", i);
    }
  }

  // 2) 先以临时频点（470.0）初始化全部检测到的射频（供信道质量测量）
  g_rfCount = 0;
  for (uint8_t j = 0; j < m; j++) {
    uint8_t i = present[j];
    RfUnit &u = g_rf[g_rfCount];
    u.nss = (uint8_t)kRfSlots[i][0];
    u.busy = (uint8_t)kRfSlots[i][1];
    u.rst = kRfSlots[i][2];
    u.dio1 = (int8_t)kRfSlots[i][3];
    u.freqMhz = TDMA_STD_BASE_MHZ;  // 临时频点，测量后重新分配
    u.gridIdx = 0;
    // DIO1：槽 0 = PIN_SX_DIO1（G04，已接线）；其余 -1（多射频没有 DIO1 引脚预算）
    u.link = RadioLink(u.nss, u.rst, u.busy, u.dio1);
    if (!u.link.begin(u.freqMhz)) continue;
    u.ok = true;
    g_rfCount++;
  }
  if (g_rfCount == 0) {
    Serial.println("[self-test] FATAL: no radio up, abort");
    // 不要静默死循环：每 2s 把原因打一遍，否则 USB-CDC 下表现为"完全没输出"
    for (;;) {
      delay(2000);
      Serial.printf("[self-test] FATAL: no radio up —— 已驱动 RST(G%u) 并发过 %ums "
                    "复位脉冲后仍无应答（本机检测到 %u 个槽位）。按序查："
                    "① 模组 3.3V（量模组 VCC-GND）；"
                    "② RES(G%u) 对 GND 静态电压应≈3.3V——若≈1.6V 说明 NRESET 仍被分压，"
                    "需在 NRESET 到 3.3V 加 10k 上拉；"
                    "③ SPI 四线通断 NSS=G%d SCK=G%d MOSI=G%d MISO=G%d；"
                    "④ 烧 spi-read 固件对照（能读到版本串=硬件没问题）\n",
                    (unsigned)PIN_SX_RST, (unsigned)kSlotRstLowMs, (unsigned)m,
                    (unsigned)PIN_SX_RST, PIN_SX_NSS, PIN_SX_SCLK, PIN_SX_MOSI,
                    PIN_SX_MISO);
    }
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

#if GW_WIFI_ENABLE
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
#else
  // WiFi/服务器整体关闭（GW_WIFI_ENABLE=0）：不连网、不阻塞开机。
  // ⚠️ 原代码这里会**阻塞等连上**（占位 SSID 永远连不上）——LoRa 联调不需要它。
  Serial.println("[wifi] disabled (GW_WIFI_ENABLE=0) —— 只跑 LoRa；"
                 "本该上报服务器的 JSON 镜像为 [up-json] 行");
#endif
  gwDisplay.begin(PIN_OLED_SDA, PIN_OLED_SCL, OLED_ADDR);
  Serial.printf("[gw] %u radios ready, gateway up\n", g_rfCount);
}

// ===== 串口状态报告（USB-CDC 下"开机打印丢失"的解药）=====
// 调用于：主机后接入时补打、以及每 5s 心跳 —— 保证任何时候打开监视器都能看到状态
static void printSerialReport() {
  Serial.printf("[gw] t=%lus rf=%u ip=%s ws=%s", (unsigned long)(millis() / 1000),
                (unsigned)g_rfCount, localIpStr(),
                g_wsConnected ? "up" : "down");
  for (uint8_t j = 0; j < g_rfCount; j++) {
    Serial.printf(" | rf%u %.1fMHz k=%u devs=%u%s", (unsigned)j,
                  (double)g_rf[j].freqMhz, (unsigned)g_rf[j].gridIdx,
                  (unsigned)g_rf[j].mac.activeCount(),
                  g_rf[j].ok ? "" : "(fail)");
  }
  Serial.println();
}

void loop() {
  // 0) 串口：主机后接入 → 补打报告；否则每 5s 一行心跳
  //    （USB-CDC 下开机打印会丢，这两条保证"任何时候打开监视器都看得到东西"）
  static bool hostSeen = false;
  static uint32_t lastHbLog = 0;
  if (!hostSeen && (bool)Serial) {
    hostSeen = true;
    printSerialReport();
  } else if ((uint32_t)(millis() - lastHbLog) >= 5000) {
    lastHbLog = millis();
    if ((bool)Serial) printSerialReport();
  }

#if GW_WIFI_ENABLE
  ws.loop();

  mcastPoll();
#endif

  // 每个射频的 TDMA 上行 → 服务器（各 MAC 任务共听时隙）
  for (uint8_t j = 0; j < g_rfCount; j++) {
    TdmaFrame f;
    while (g_rf[j].mac.pollUplink(f)) {
      onTdmaUplink(j, f);
    }
  }

  // 状态灯（G48 模组板载单色 LED，低电平点亮 → 走 statusLedWrite，别直写电平）：
  // 服务器在线常亮；离线 300ms 闪 —— **闪 = 程序在跑**（不接服务器时就是常态）
  if (millis() - g_lastLedToggle > (g_wsConnected ? 2000 : 300)) {
    g_lastLedToggle = millis();
    g_ledOn = g_wsConnected ? true : !g_ledOn;
    statusLedWrite(g_ledOn);
  }

  // 显示屏：连接状态 + 全信道设备总数
  uint8_t total = 0;
  for (uint8_t j = 0; j < g_rfCount; j++) total += g_rf[j].mac.activeCount();
  gwDisplay.update(g_wsConnected, total, localIpStr());
}