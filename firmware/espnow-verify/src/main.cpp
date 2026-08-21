// ============================================================
// espnow-verify：ESP-NOW 信号验证固件（接收电流检测板信号）
//
// 接收检测板（firmware/trigger-board）上报的开火/心跳/异常帧，
// 串口实时显示 + 统计（射速/丢帧/RSSI/发送端）+ 下行断电/恢复测试。
// 用法见 README.md；输出均带 #TAG 前缀，便于脚本解析。
// ============================================================
#include <Arduino.h>
#include "config.h"
#include "EspNowRx.h"

EspNowRx rxLink;

static const uint8_t kBoardMac[6] = BOARD_MAC;

// ---- 帧计数 ----
static uint32_t g_total = 0, g_fire = 0, g_hb = 0, g_fault = 0, g_other = 0;

// ---- 开火统计 ----
static uint32_t g_fireRing[FIRE_RING_SIZE];    // 最近开火时刻（ms）
static uint8_t  g_fireRingIdx = 0;
static uint32_t g_fireRingCount = 0;
static uint32_t g_lastFireMs = 0;
static uint32_t g_fireIntervalMin = 0, g_fireIntervalSum = 0, g_fireIntervalN = 0;
static uint8_t  g_lastFireSeq = 0;
static bool     g_haveFireSeq = false;

// ---- 心跳 / 在线 ----
static uint32_t g_lastHbMs = 0;
static uint8_t  g_lastHbCount = 0;
static bool     g_hbValid = false;             // 已见过首帧心跳
static bool     g_online = false;
static uint32_t g_hbGapTotal = 0;              // 累计丢包（HB 计数跳变 >1）

// ---- 故障 ----
static uint8_t  g_lastFaultCode = 0;
static uint32_t g_lastFaultMs = 0;
static bool     g_faultLed = false;            // 故障 LED 常亮（'r' 熄灭）

// ---- RSSI ----
static int8_t g_lastRssi = 0;
static int8_t g_rssiMin = 0, g_rssiMax = 0;
static bool   g_haveRssi = false;

// ---- 发送端列表（多板同测时区分）----
struct Sender { uint8_t mac[6]; uint32_t frames; uint32_t lastMs; };
static Sender g_senders[SENDER_MAX];
static uint8_t g_senderCount = 0;

// ---- 其它 ----
static bool     g_verbose = false;
static bool     g_powerOn = true;              // 下行测试记住上次状态（默认导通）
static uint32_t g_ledUntil = 0;
static uint32_t g_lastStatus = 0;

// ===== 工具 =====
static uint32_t fireRate1s(uint32_t nowMs) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < g_fireRingCount; i++)
    if (nowMs - g_fireRing[i] <= FIRE_RATE_WINDOW_MS) n++;
  return n;
}

static void noteFire(uint32_t nowMs) {
  g_fireRing[g_fireRingIdx] = nowMs;
  g_fireRingIdx = (g_fireRingIdx + 1) % FIRE_RING_SIZE;
  if (g_fireRingCount < FIRE_RING_SIZE) g_fireRingCount++;
  if (g_lastFireMs) {
    const uint32_t dt = nowMs - g_lastFireMs;
    if (!g_fireIntervalMin || dt < g_fireIntervalMin) g_fireIntervalMin = dt;
    g_fireIntervalSum += dt;
    g_fireIntervalN++;
  }
  g_lastFireMs = nowMs;
}

static void noteSender(const uint8_t *mac, uint32_t nowMs) {
  for (uint8_t i = 0; i < g_senderCount; i++) {
    if (memcmp(g_senders[i].mac, mac, 6) == 0) {
      g_senders[i].frames++;
      g_senders[i].lastMs = nowMs;
      return;
    }
  }
  if (g_senderCount < SENDER_MAX) {
    memcpy(g_senders[g_senderCount].mac, mac, 6);
    g_senders[g_senderCount].frames = 1;
    g_senders[g_senderCount].lastMs = nowMs;
    g_senderCount++;
  }
}

// ===== 帧处理 =====
static void handleFrame(const RxFrame &f, uint32_t nowMs) {
  if (g_verbose) {
    Serial.printf("#RAW from=%02X:%02X:%02X:%02X:%02X:%02X len=%d rssi=%d data=",
                  f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5],
                  f.len, (int)f.rssi);
    for (int i = 0; i < f.len; i++) Serial.printf("%02X ", f.data[i]);
    Serial.println();
  }

  // 非本协议帧（magic 不符 / 长度不足）
  if (f.len < 4 || f.data[0] != FRAME_MAGIC) {
    g_other++;
    Serial.printf("#BAD len=%d first=0x%02X from=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  f.len, f.len ? f.data[0] : 0,
                  f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5]);
    return;
  }

  const uint8_t type = f.data[1], d0 = f.data[2], d1 = f.data[3];
  g_total++;
  noteSender(f.mac, nowMs);

  // RSSI 统计（旧内核回调为 0，忽略）
  if (f.rssi != 0) {
    g_lastRssi = f.rssi;
    if (!g_haveRssi) { g_rssiMin = g_rssiMax = f.rssi; g_haveRssi = true; }
    else {
      if (f.rssi < g_rssiMin) g_rssiMin = f.rssi;
      if (f.rssi > g_rssiMax) g_rssiMax = f.rssi;
    }
  }

  switch (type) {
    case FRAME_FIRE: {
      g_fire++;
      noteFire(nowMs);
      const uint8_t seq = d0;
      if (g_haveFireSeq) {
        const uint8_t expect = (uint8_t)(g_lastFireSeq + 1);
        if (seq != expect)
          Serial.printf("#FIRE_GAP seq=%u expect=%u (丢帧?)\n",
                        (unsigned)seq, (unsigned)expect);
      }
      g_lastFireSeq = seq;
      g_haveFireSeq = true;
      digitalWrite(PIN_LED, HIGH);             // 每发闪灯
      g_ledUntil = nowMs + FIRE_LED_MS;
      Serial.printf("#FIRE seq=%u rssi=%d rate1s=%lu\n",
                    (unsigned)seq, (int)f.rssi, (unsigned long)fireRate1s(nowMs));
      break;
    }

    case FRAME_HB: {
      g_hb++;
      const uint8_t hc = d0;
      const uint32_t intervalMs = g_hbValid ? (nowMs - g_lastHbMs) : 0;
      if (g_hbValid) {
        const uint8_t d = (uint8_t)(hc - g_lastHbCount);   // 模 256 差值
        if (d == 1) {
          // 正常 +1
        } else if (d == 0) {
          // 重复帧（罕见）
        } else if (d > 90) {
          Serial.printf("#HB_RESET n=%u (检测板重启?)\n", (unsigned)hc);
        } else {
          const uint32_t lost = (uint32_t)(d - 1);
          g_hbGapTotal += lost;
          Serial.printf("#HB_GAP lost=%lu n=%u\n", (unsigned long)lost, (unsigned)hc);
        }
      }
      g_lastHbCount = hc;
      g_lastHbMs = nowMs;
      g_hbValid = true;
      Serial.printf("#HB n=%u interval=%lums rssi=%d\n",
                    (unsigned)hc, (unsigned long)intervalMs, (int)f.rssi);
      break;
    }

    case FRAME_FAULT:
      g_fault++;
      g_lastFaultCode = d0;
      g_lastFaultMs = nowMs;
      g_faultLed = true;                       // LED 常亮，'r' 熄灭
      digitalWrite(PIN_LED, HIGH);
      Serial.printf("#FAULT code=0x%02X rssi=%d (检测板已自动断电; 'r' 熄灭 LED)\n",
                    (unsigned)d0, (int)f.rssi);
      break;

    default:
      g_other++;
      Serial.printf("#UNKNOWN type=0x%02X d0=%u d1=%u from=%02X:%02X:%02X:%02X:%02X:%02X\n",
                    (unsigned)type, (unsigned)d0, (unsigned)d1,
                    f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5]);
      break;
  }
}

// ===== 在线状态（心跳 3s 超时判离线，变化时打印）=====
static void updateOnline(uint32_t nowMs) {
  if (!g_hbValid) return;
  const bool on = (nowMs - g_lastHbMs) < HB_TIMEOUT_MS;
  if (on != g_online) {
    g_online = on;
    Serial.printf("#LINK %s (age=%lums)\n", on ? "online" : "offline",
                  (unsigned long)(nowMs - g_lastHbMs));
  }
}

// ===== 状态打印 =====
static void printMacInfo() {
  uint8_t cur[6] = {0}, fac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, cur);
  esp_efuse_mac_get_default(fac);
  Serial.printf("#MAC own=%02X:%02X:%02X:%02X:%02X:%02X factory=%02X:%02X:%02X:%02X:%02X:%02X spoof=%u ch=%u\n",
                cur[0], cur[1], cur[2], cur[3], cur[4], cur[5],
                fac[0], fac[1], fac[2], fac[3], fac[4], fac[5],
                (unsigned)rxLink.macSpoofed(), ESPNOW_CHANNEL);
  Serial.printf("#MAC power_target=%02X:%02X:%02X:%02X:%02X:%02X unicast=%u\n",
                kBoardMac[0], kBoardMac[1], kBoardMac[2],
                kBoardMac[3], kBoardMac[4], kBoardMac[5],
                (unsigned)(kBoardMac[0] != 0xFF));
}

static void printStatus(uint32_t nowMs) {
  const uint32_t age = g_hbValid ? (nowMs - g_lastHbMs) : 0;
  const uint32_t ivAvg = g_fireIntervalN ? g_fireIntervalSum / g_fireIntervalN : 0;
  Serial.printf("#STAT online=%u uptime=%lus total=%lu fire=%lu hb=%lu fault=%lu other=%lu "
                "rate1s=%lu hb_age=%lums hb_lost=%lu fire_min=%lums fire_avg=%lums "
                "last_rssi=%d rssi_min=%d rssi_max=%d tx_ok=%lu tx_fail=%lu fault_last=0x%02X\n",
                (unsigned)g_online, (unsigned long)(nowMs / 1000UL),
                (unsigned long)g_total, (unsigned long)g_fire, (unsigned long)g_hb,
                (unsigned long)g_fault, (unsigned long)g_other,
                (unsigned long)fireRate1s(nowMs), (unsigned long)age,
                (unsigned long)g_hbGapTotal,
                (unsigned long)g_fireIntervalMin, (unsigned long)ivAvg,
                (int)g_lastRssi, (int)g_rssiMin, (int)g_rssiMax,
                (unsigned long)rxLink.txOk(), (unsigned long)rxLink.txFail(),
                (unsigned)g_lastFaultCode);
  for (uint8_t i = 0; i < g_senderCount; i++) {
    Serial.printf("#SENDER %u mac=%02X:%02X:%02X:%02X:%02X:%02X frames=%lu last=%lums\n",
                  (unsigned)i,
                  g_senders[i].mac[0], g_senders[i].mac[1], g_senders[i].mac[2],
                  g_senders[i].mac[3], g_senders[i].mac[4], g_senders[i].mac[5],
                  (unsigned long)g_senders[i].frames,
                  (unsigned long)(nowMs - g_senders[i].lastMs));
  }
}

// ===== 串口命令 =====
static void printHelp() {
  Serial.println("#commands:");
  Serial.println("#  s        状态快照");
  Serial.println("#  r        清零统计 + 熄灭故障 LED");
  Serial.println("#  v        切换详细模式（打印每帧原始数据）");
  Serial.println("#  p0 / p1  下行测试：发 FRAME_POWER 断电(0)/恢复(1)");
  Serial.println("#  p        切换断电/恢复（上次相反）");
  Serial.println("#  m        打印本机/配对信息");
  Serial.println("#  ? / h    帮助");
}

static void resetStats() {
  g_total = g_fire = g_hb = g_fault = g_other = 0;
  g_fireRingIdx = 0; g_fireRingCount = 0; g_lastFireMs = 0;
  g_fireIntervalMin = 0; g_fireIntervalSum = 0; g_fireIntervalN = 0;
  g_lastFireSeq = 0; g_haveFireSeq = false;
  g_lastHbMs = 0; g_lastHbCount = 0; g_hbValid = false; g_hbGapTotal = 0;
  g_lastFaultCode = 0; g_lastFaultMs = 0;
  g_lastRssi = 0; g_haveRssi = false;
  g_senderCount = 0;
  g_faultLed = false; g_ledUntil = 0;
  digitalWrite(PIN_LED, LOW);
  Serial.println("#RESET stats cleared");
}

static void sendPowerCmd(bool on) {
  rxLink.sendPower(on);
  g_powerOn = on;
  Serial.printf("#TX FRAME_POWER=%s sent (ACK 结果见 #STAT 的 tx_ok/tx_fail)\n",
                on ? "ON" : "OFF");
}

static void runCommand(char *cmd) {
  const char c = tolower(cmd[0]);
  switch (c) {
    case '?': case 'h': printHelp(); break;
    case 's': printStatus(millis()); break;
    case 'r': resetStats(); break;
    case 'v':
      g_verbose = !g_verbose;
      Serial.printf("#VERBOSE=%u\n", (unsigned)g_verbose);
      break;
    case 'm': printMacInfo(); break;
    case 'p':
      if (cmd[1] == '0') sendPowerCmd(false);
      else if (cmd[1] == '1') sendPowerCmd(true);
      else sendPowerCmd(!g_powerOn);
      break;
    default:
      Serial.printf("#unknown: %s\n", cmd);
      printHelp();
  }
}

static void handleSerial() {
  static char buf[32];
  static uint8_t len = 0;
  while (Serial.available()) {
    const char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (len) { buf[len] = '\0'; runCommand(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = ch;
    }
  }
}

// ===== 主流程 =====
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.println();
  Serial.println("===== espnow-verify : 电流检测板 ESP-NOW 信号验证 =====");
  if (!rxLink.begin())
    Serial.println("#ERROR esp_now_init failed");
  printMacInfo();
  Serial.printf("#FRAME protocol: magic=0x%02X fire=0x%02X hb=0x%02X fault=0x%02X power=0x%02X ch=%u\n",
                FRAME_MAGIC, FRAME_FIRE, FRAME_HB, FRAME_FAULT, FRAME_POWER, ESPNOW_CHANNEL);
  Serial.println("#waiting for board frames (expect HB every 1s)...");
  Serial.println("#type '?' for commands");
}

void loop() {
  const uint32_t now = millis();

  // 1) 取接收帧并处理
  RxFrame f;
  while (rxLink.received(f)) handleFrame(f, now);

  // 2) 心跳超时 → 在线/离线
  updateOnline(now);

  // 3) LED：开火闪 FIRE_LED_MS；FAULT 常亮（'r' 熄灭）
  if (!g_faultLed && now >= g_ledUntil) digitalWrite(PIN_LED, LOW);

  // 4) 周期状态
  if (now - g_lastStatus >= STATUS_INTERVAL_MS) {
    g_lastStatus = now;
    printStatus(now);
  }

  // 5) 串口命令
  handleSerial();

  delay(1);
}
