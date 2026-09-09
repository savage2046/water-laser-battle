#include <Arduino.h>
#include <SPI.h>
#include <stdarg.h>
#include <RadioLib.h>
#include "EspNowAuto.h"

// ============================================================================
// gun-selftest —— 枪端主控板硬件体检固件（LCKFB-ESP32S3R8N8 / ESP32-S3）
//
// T1 板载 LED 闪烁（永远运行 = 程序活着；兼做结果灯）
// T2 LoRa 模块连接检测（U2 Ra-01S / SX1268）：开机跑一次，判 PASS/FAIL
// T3 ESP-NOW 自动连接：开机先连「上次成功地址」(NVS)，3s 未连上则扫描附近
//    (promiscuous 抓包按 RSSI) 选信号最强探测连接；成功后地址写 NVS。
//
// 串口策略（USB CDC 后插/后开监视器也能看到）：
//   打印全部经 logf() —— 无 USB 主机时自动静默、不阻塞（可电池单供）；
//   主机接入瞬间自动补打完整报告，首个 5s 心跳周期再复述一次（防枚举丢帧）；
//   之后每 5s 一行 #hb 心跳。
// ============================================================================

// ---------- T1：板载 LED（GPIO48 = 模组丝印 G48，低电平点亮）----------
#define PIN_LED 48
#define LED_OK_HALF_MS 500          // 全 PASS：亮0.5s/灭0.5s（每秒 1 次）
#define LED_ESPNOW_FAIL_HALF_MS 260 // 仅 ESP-NOW FAIL：中速闪
#define LED_LORA_FAIL_HALF_MS 120   // LoRa FAIL：快闪
#define SERIAL_BAUD 115200

// ---------- T2：LoRa（U2 Ra-01S / SX1268，SPI 六线，见原理图在线读取）----------
#define PIN_SX_NSS 16         // CS（DIP35=G16）
#define PIN_SX_SCLK 42        // SCK（DIP38=G42）
#define PIN_SX_MOSI 15        // MOSI（DIP36=G15）
#define PIN_SX_MISO 41        // MISO（DIP37=G41）
#define PIN_SX_RST 46         // RaRES（DIP40=G46，低有效复位）
#define PIN_SX_BUSY 45        // RaBUSY（DIP39=G45）

#define RADIO_FREQ_MHZ 470.0f
#define RADIO_BW_KHZ 500.0f
#define RADIO_SF 7
#define RADIO_CR 5
#define RADIO_SYNC_WORD 0x12
#define RADIO_TX_POWER_DBM 15
#define RADIO_PREAMBLE_LEN 4

// ---------- T3：ESP-NOW（信道 1，与检测板/espnow-verify 一致）----------
#define ESPNOW_CHANNEL 1

// ---------- 状态 ----------
static bool    g_loraPass  = false;   // T2
static int     g_loraState = 0;
static int16_t g_rssi      = 0;
static bool    g_t2Ran     = false;

static EspNowAuto g_esp;              // T3
static bool    g_espPass   = false;
static int     g_espCode   = EspNowAuto::RC_SCAN_NONE;
static bool    g_t3Ran     = false;

static uint16_t g_ledHalfMs = LED_LORA_FAIL_HALF_MS;  // 由测试结论设定
static bool    g_hostSeen  = false;
static bool    g_reportPending = true;  // 报告待复述（枚举窗口打印可能丢帧）

// 串口：主机在才打（USB CDC 枚举/终端打开前自动静默，不阻塞）
static void logf(const char *fmt, ...) {
  if (!(bool)Serial) return;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

// ---------- 工具 ----------
static const char* loraErrName(int code) {
  switch (code) {
    case RADIOLIB_ERR_NONE:            return "RADIOLIB_ERR_NONE（无错误）";
    case RADIOLIB_ERR_UNKNOWN:         return "UNKNOWN（未知错误）";
    case RADIOLIB_ERR_CHIP_NOT_FOUND:  return "CHIP_NOT_FOUND：芯片无应答/不在位 —— 查 CS/MISO/BUSY/供电/焊接";
    case RADIOLIB_ERR_SPI_WRITE_FAILED:return "SPI_WRITE_FAILED：SPI 读写回读不一致 —— 查 MOSI/MISO/SCLK/焊接";
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT: return "SPI_CMD_TIMEOUT：命令超时 —— 查 BUSY(G45)";
    case RADIOLIB_ERR_SPI_CMD_INVALID: return "SPI_CMD_INVALID";
    case RADIOLIB_ERR_SPI_CMD_FAILED:  return "SPI_CMD_FAILED";
    case RADIOLIB_ERR_TX_TIMEOUT:      return "TX_TIMEOUT";
    case RADIOLIB_ERR_RX_TIMEOUT:      return "RX_TIMEOUT";
    case RADIOLIB_ERR_INVALID_BANDWIDTH:        return "INVALID_BANDWIDTH";
    case RADIOLIB_ERR_INVALID_FREQUENCY:        return "INVALID_FREQUENCY";
    default: return "其它错误（见 RadioLib TypeDef.h）";
  }
}

static const char* espCodeName(int code) {
  switch (code) {
    case EspNowAuto::RC_CONNECTED:      return "已连接";
    case EspNowAuto::RC_RECONNECT_FAIL: return "上次地址 3s 未连上，且扫描未发现附近设备帧";
    case EspNowAuto::RC_SCAN_NONE:      return "无已保存地址，附近也未侦听到 ESP-NOW 设备帧（对端需上电并在信道1收发）";
    case EspNowAuto::RC_SCAN_FAIL:      return "扫描到候选但逐个探测均无链路层 ACK";
    case EspNowAuto::RC_INIT_FAIL:      return "esp_now_init 失败（WiFi 栈异常）";
    default: return "未知";
  }
}

static void macStr(const uint8_t *m, char *out) {
  if (!m) { out[0] = '-'; out[1] = '\0'; return; }
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// 完整报告：串口刚接入 / 首个心跳周期复述，都从这里出
static void printFullReport() {
  logf("\n[gun-selftest] ====== 自检报告 (v0.3.0, 编译 %s %s) ======\n",
       __DATE__, __TIME__);
  logf("  MCU = %s, Flash = %u MB, SDK = %s\n",
       ESP.getChipModel(), ESP.getFlashChipSize() / (1024 * 1024),
       ESP.getSdkVersion());

  logf("  [T1] 板载 LED (GPIO48/G48) —— 程序运行中\n");

  if (!g_t2Ran) {
    logf("  [T2] LoRa 尚未检测\n");
  } else if (g_loraPass) {
    logf("  [T2] LoRa (U2 Ra-01S/SX1268, SPI CS=G%d SCK=G%d MOSI=G%d MISO=G%d | RST=G%d BUSY=G%d)\n",
         PIN_SX_NSS, PIN_SX_SCLK, PIN_SX_MOSI, PIN_SX_MISO, PIN_SX_RST, PIN_SX_BUSY);
    logf("       begin(%.1fMHz,BW%.0fk,SF%d,CR4/%d,sync=0x%02X)=0 OK；空闲底噪 RSSI=%d dBm\n",
         (double)RADIO_FREQ_MHZ, (double)RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
         RADIO_SYNC_WORD, g_rssi);
    logf("#RESULT lora=PASS\n");
  } else {
    logf("  [T2] LoRa begin 返回 %d (%s)\n", g_loraState, loraErrName(g_loraState));
    logf("#RESULT lora=FAIL (begin=%d)\n", g_loraState);
  }

  if (!g_t3Ran) {
    logf("  [T3] ESP-NOW 尚未检测\n");
  } else if (g_espPass) {
    char pm[18];
    macStr(g_esp.peer(), pm);
    logf("  [T3] ESP-NOW 自动连接：%s (rssi=%d dBm, ack=%lu rx=%lu)\n",
         pm, (int)g_esp.peerRssi(),
         (unsigned long)g_esp.ackOk(), (unsigned long)g_esp.rxTotal());
    if (g_esp.usedSavedPeer()) {
      const uint8_t *s = g_esp.savedMac();
      if (s) logf("       命中「上次成功地址」%02X:%02X:…:%02X\n", s[0], s[1], s[5]);
    } else if (g_esp.candCount() > 0) {
      logf("       扫描候选 %u 个 → 取信号最强连接；已保存下次优先\n", g_esp.candCount());
    }
    logf("#RESULT espnow=PASS\n");
  } else {
    logf("  [T3] ESP-NOW: %s\n", espCodeName(g_espCode));
    for (uint16_t i = 0; i < g_esp.candCount() && i < 5; i++) {
      const EsCandidate *c = g_esp.candidate(i);
      if (!c) break;
      char cm[18];
      macStr(c->mac, cm);
      logf("       候选[%u] %s rssi=%d dBm 帧=%u\n", i, cm, (int)c->rssi, c->frames);
    }
    logf("#RESULT espnow=FAIL (code=%d)\n", g_espCode);
  }

  logf("  LED 节奏：每秒1次=全PASS；中速闪=仅ESP-NOW FAIL；快闪=LoRa FAIL\n");
  logf("#ready 复位可重跑全部自检；输入 x 清除已保存地址后重启\n");
}

// ---------------- T2：LoRa 模块连接检测 ----------------
static void runTestLora() {
  g_t2Ran = true;
  SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);

  // DIO1 未接线 → RADIOLIB_NC（RadioLib 轮询 BUSY 模式，与正式固件相同）
  static Module mod(PIN_SX_NSS, RADIOLIB_NC, PIN_SX_RST, PIN_SX_BUSY);
  static SX1268 radio(&mod);

  g_loraState = radio.begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                            RADIO_SYNC_WORD, RADIO_TX_POWER_DBM,
                            RADIO_PREAMBLE_LEN, 0);
  if (g_loraState == RADIOLIB_ERR_NONE) {
    radio.startReceive(200);
    g_rssi = radio.getRSSI();
    g_loraPass = true;
  }
}

// ---------------- T3：ESP-NOW 自动连接 ----------------
static void runTestEspNow() {
  g_t3Ran = true;
  g_esp.begin(ESPNOW_CHANNEL);
  g_espCode = g_esp.run(3000);      // 上次地址窗口 3s，超时自动扫描
  g_espPass = (g_espCode == EspNowAuto::RC_CONNECTED);
}

// 串口命令：x = 清除「上次成功地址」并重启（便于反复测扫描分支）
static void handleSerialCmd() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == 'x' || c == 'X') {
      logf("#peer 已清除，重启后将从扫描开始…\n");
      delay(300);
      ESP.restart();
    }
  }
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);      // 灭

  Serial.begin(SERIAL_BAUD);        // 原生 USB CDC（依赖 ARDUINO_USB_CDC_ON_BOOT=1）
  delay(200);

  // 上电三闪：不接串口也能确认代码已启动
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, LOW);  delay(120);
    digitalWrite(PIN_LED, HIGH); delay(120);
  }

  runTestLora();
  runTestEspNow();

  // LED 结果节奏：全过最慢；仅 ESP-NOW 挂中速；LoRa 挂快闪
  if (g_loraPass && g_espPass)       g_ledHalfMs = LED_OK_HALF_MS;
  else if (g_loraPass && !g_espPass) g_ledHalfMs = LED_ESPNOW_FAIL_HALF_MS;
  else                               g_ledHalfMs = LED_LORA_FAIL_HALF_MS;

  printFullReport();                // 主机已在则立刻可见
}

void loop() {
  uint32_t now = millis();

  // 主机后接入 → 补打一次完整报告（等 300ms 让 CDC 端点就绪，避免丢帧）
  if (!g_hostSeen && (bool)Serial) {
    g_hostSeen = true;
    delay(300);
    printFullReport();
  }

  // LED 节奏（按测试结论的半周期）
  static uint32_t nextToggle = 0;
  static bool ledOn = false;
  if (now >= nextToggle) {
    ledOn = !ledOn;
    digitalWrite(PIN_LED, ledOn ? LOW : HIGH);   // 低电平点亮
    nextToggle = now + g_ledHalfMs;
  }

  // 心跳：接入后每 5s 一行，随时可确认程序活着
  static uint32_t lastHb = 0;
  if (now - lastHb >= 5000) {
    lastHb = now;
    // 首个心跳周期复述完整报告：开机瞬间/枚举窗口的打印可能已被丢，
    // 而心跳已被证实能收到，借同一时机补发报告必达
    if (g_reportPending) {
      g_reportPending = false;
      printFullReport();
    }
    char pm[18];
    macStr(g_esp.peer(), pm);
    logf("#hb t=%lus lora=%s esp=%s peer=%s rssi=%d\n", now / 1000,
         g_loraPass ? "PASS" : "FAIL", g_espPass ? "PASS" : "FAIL", pm,
         g_espPass ? (int)g_esp.peerRssi() : 0);
  }

  handleSerialCmd();
}
