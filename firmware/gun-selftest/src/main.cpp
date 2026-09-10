#include <Arduino.h>
#include <SPI.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "EspNowAuto.h"
#include "LoRaDev.h"
#include "IrAlign.h"

// ============================================================================
// gun-selftest —— 枪端主控板硬件体检 + 联机联调固件（LCKFB-ESP32S3R8N8）
//
// T1 板载 LED 闪烁（永远运行 = 程序活着；兼做结果灯与光路提示）
// T2 LoRa 模块连接检测（U2 Ra-01S / SX1268）：开机跑一次，判 PASS/FAIL
// T3 ESP-NOW 自动连接（检测板/音效联动）：先连 NVS 里上次成功的地址，3s 超时扫频选最强
// T4 ★ LoRa 网关自动连接 + **每秒心跳**：扫 20 个标准频点找网关信标 → 注册槽发 JOIN
//      → 拿到 TF_ASSIGN（信道/时隙）→ 之后**每 1000ms 在自身时隙发一个 TF_HB**
// T5 ★ 940nm/38kHz 光路矫正发射：**每 1000ms 发一帧最低功率的编码信号**
//      （近距 G47 / 远距 G13 可选，默认两个都发），用于对准接收头/校准靶/检查光路
//
// 串口策略（USB CDC 后插/后开监视器也能看到）：
//   打印全部经 dsLog() —— 无 USB 主机时自动静默、不阻塞（可电池单供）；
//   主机接入瞬间自动补打完整报告，首个 5s 心跳周期再复述一次（防枚举丢帧）；
//   之后每 5s 一行 #hb。
//
// 命令：h 帮助 | i 报告 | r 清统计 | x 重启 | j 立刻补发 JOIN | s 重扫网关
//       k<0-19> 手动锁定频点 | I 暂停/恢复光路矫正发射 | b 立刻发一帧
//       c0/c1/c2 选发射通道
// ============================================================================

static bool     g_hostSeen = false;
static bool     g_reportPending = true;
static uint32_t g_bootMs = 0;
static uint32_t g_lastIrMs = 0;

static EspNowAuto g_esp;              // T3
static bool    g_espPass = false;
static int     g_espCode = EspNowAuto::RC_SCAN_NONE;
static bool    g_t3Ran = false;

// ---------------------------------------------------------------- 串口
void dsLog(const char *fmt, ...) {
  if (!(bool)Serial) return;   // 无主机：静默（不阻塞、可电池单供）
  char buf[224];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

static const char *loraErrName(int code) {
  switch (code) {
    case RADIOLIB_ERR_NONE:             return "RADIOLIB_ERR_NONE（无错误）";
    case RADIOLIB_ERR_CHIP_NOT_FOUND:   return "CHIP_NOT_FOUND：芯片无应答/不在位 —— 查 CS/MISO/BUSY/供电/焊接";
    case RADIOLIB_ERR_SPI_WRITE_FAILED: return "SPI_WRITE_FAILED：SPI 读写回读不一致 —— 查 MOSI/MISO/SCLK/焊接";
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT:  return "SPI_CMD_TIMEOUT：命令超时 —— 查 BUSY(G45)";
    case RADIOLIB_ERR_SPI_CMD_INVALID:  return "SPI_CMD_INVALID";
    case RADIOLIB_ERR_SPI_CMD_FAILED:   return "SPI_CMD_FAILED";
    case RADIOLIB_ERR_TX_TIMEOUT:       return "TX_TIMEOUT";
    case RADIOLIB_ERR_RX_TIMEOUT:       return "RX_TIMEOUT";
    default:                            return "其它错误（见 RadioLib TypeDef.h）";
  }
}

static const char *espCodeName(int code) {
  switch (code) {
    case EspNowAuto::RC_CONNECTED:      return "已连接";
    case EspNowAuto::RC_RECONNECT_FAIL: return "上次地址 3s 未连上，且扫描未发现附近设备帧";
    case EspNowAuto::RC_SCAN_NONE:      return "无已保存地址，附近也未侦听到 ESP-NOW 设备帧（对端需上电并在信道1收发）";
    case EspNowAuto::RC_SCAN_FAIL:      return "扫描到候选但逐个探测均无链路层 ACK";
    case EspNowAuto::RC_INIT_FAIL:      return "esp_now_init 失败（WiFi 栈异常）";
    default:                            return "未知";
  }
}

static void macStr(const uint8_t *m, char *out) {
  if (!m) { out[0] = '-'; out[1] = '\0'; return; }
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// ---------------------------------------------------------------- 报告
static void printT4() {
  const DevLink &L = g_dev.link();
  dsLog("  [T4] LoRa 网关自动连接 + 1s 心跳\n");
  if (!L.ready) {
    dsLog("       射频未就绪（T2 FAIL），本项无法进行\n");
    return;
  }
  dsLog("       状态 = %s\n",
        L.assigned ? "已连上网关（已获信道+时隙）"
                   : (L.locked ? "已锁定网关信标，注册中" : "未连上（扫描/等待网关）"));
  dsLog("       网关频点 k=%u (%.1fMHz)  我的时隙 slot=%u  N=%u  mapVer=%u\n",
        (unsigned)L.grid,
        (double)(TDMA_STD_BASE_MHZ + (float)L.grid * TDMA_STD_STEP_MHZ),
        (unsigned)L.slot, (unsigned)L.n, (unsigned)L.mapVer);
  dsLog("       信标：收到 %lu 个；RSSI %d dBm  SNR %.2f dB  载偏 %d Hz；最近 %lums 前\n",
        (unsigned long)L.beacons, (int)L.beaconRssi,
        (double)L.beaconSnrX4 / 4.0, (int)L.beaconFeHz,
        (unsigned long)(L.lastBeaconMs ? (millis() - L.lastBeaconMs) : 0));
  dsLog("       注册：扫频 %lu 轮  JOIN %lu 次  ASSIGN %lu 次  注册时延 %lums\n",
        (unsigned long)L.scans, (unsigned long)L.joins, (unsigned long)L.assigns,
        (unsigned long)L.regLatMs);
  dsLog("       ★心跳：已发 %lu 次（入队 %lu）  迟到 %lu 次\n",
        (unsigned long)L.hbTx, (unsigned long)L.hbQueued, (unsigned long)L.hbLate);
  if (L.hbIntN) {
    dsLog("             发送间隔 min/avg/max = %lu/%lu/%lu ms（n=%u，目标 %dms）\n",
          (unsigned long)L.hbIntMin, (unsigned long)(L.hbIntSum / L.hbIntN),
          (unsigned long)L.hbIntMax, (unsigned)L.hbIntN, HB_PERIOD_MS);
  }
  if (L.hbOffN) {
    dsLog("             时隙对齐偏移 min/max/avg = %ld/%ld/%ld us（相对期望时隙起点）\n",
          (long)L.hbOffMin, (long)L.hbOffMax, (long)(L.hbOffSum / (int32_t)L.hbOffN));
  }
  dsLog("             下行收到 %lu 帧；坏帧 %lu；发射失败 %lu；上帧空口 %luus（理论 %luus）\n",
        (unsigned long)L.dlRx, (unsigned long)L.rxBad, (unsigned long)L.txFail,
        (unsigned long)L.airUsLast, (unsigned long)g_dev.theoreticalAirUs());
#if PIN_SX_DIO1 >= 0
  dsLog("             DIO1=G%d 中断收包：到达时刻 ±µs（已触发 %lu 次）—— 信标相位/时隙偏移以此为基准\n",
        PIN_SX_DIO1, (unsigned long)g_dev.irqCount());
#else
  dsLog("             DIO1 未接线：到达时刻取轮询时刻（相位/偏移误差可达 ~1ms）\n");
#endif
  dsLog("#RESULT gateway=%s (hb=%lu)\n",
        L.assigned ? "PASS" : (L.locked ? "JOINING" : "FAIL"),
        (unsigned long)L.hbTx);
}

static void printT5() {
  uint8_t b[5];
  g_ir.frameBytes(b);
  dsLog("  [T5] 940nm / 38kHz 光路矫正发射（最低功率）\n");
  dsLog("       载波半周期 %dus → %.2fkHz；功率档 %d（最低：近距 00 = 0.5×I_nom）\n",
        IR_ALIGN_HALF_US, 1000.0 / (2.0 * (double)IR_ALIGN_HALF_US),
        IR_ALIGN_POWER_LEVEL);
  dsLog("       节奏 = 每 %dms 一个周期，光信号持续 %dms（亮→灭 %dms）\n",
        IR_ALIGN_PERIOD_MS, IR_ALIGN_ON_MS, IR_ALIGN_PERIOD_MS - IR_ALIGN_ON_MS);
  dsLog("       发射通道 = %s；已发 %lu 帧；上一周期：亮 %lums 内发 %lu 帧，"
        "单帧耗时 %luus\n",
        g_ir.channel() == 0 ? "近距(G47)"
                            : (g_ir.channel() == 1 ? "远距(G13)" : "近距+远距(G47+G13)"),
        (unsigned long)g_ir.txCount(), (unsigned long)g_ir.lastOnMs(),
        (unsigned long)g_ir.framesPerOn(), (unsigned long)g_ir.lastBurstUs());
  dsLog("       编码帧（与 LaserCodec 逐位一致）：%02X %02X %02X %02X %02X"
        "  playerId=%u weapon=%u team=%u shotSeq=%u chk=%02X\n",
        b[0], b[1], b[2], b[3], b[4], (unsigned)((b[0] << 8) | b[1]),
        (unsigned)((b[2] >> 4) & 0x0F), (unsigned)((b[2] >> 2) & 0x03),
        (unsigned)b[3], b[4]);
  dsLog("       发射状态 = %s\n", g_ir.enabled() ? "运行中" : "已暂停（命令 I 恢复）");
}

static void printFullReport() {
  dsLog("\n[gun-selftest] ====== 自检报告 (v%s, 编译 %s %s) ======\n", FW_VERSION,
        __DATE__, __TIME__);
  dsLog("  MCU = %s, Flash = %u MB, SDK = %s, 运行 = %lus\n", ESP.getChipModel(),
        (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)), ESP.getSdkVersion(),
        (unsigned long)((millis() - g_bootMs) / 1000));

  dsLog("  [T1] 板载 LED (GPIO%d) —— %s\n", PIN_LED,
        g_dev.link().assigned ? "慢闪=已连上网关" : "中速闪=未连网关");

  const DevLink &L = g_dev.link();
  if (!L.ready) {
    dsLog("  [T2] LoRa 初始化失败：begin=%d（%s）\n", (int)L.beginCode,
          loraErrName(L.beginCode));
    dsLog("       ⚠️ 已自动试过 SX1262/SX1268/LLCC68 三个类（按版本字符串校验身份），\n");
    dsLog("          所以不是类选错，而是芯片没应答 → 查 U2 供电/CS(G16)/BUSY(G45)/\n");
    dsLog("          RST(G46)/SPI(G42/G15/G41) 焊接与 U2 是否贴反。\n");
    dsLog("#RESULT lora=FAIL (begin=%d)\n", (int)L.beginCode);
  } else {
    const uint8_t *v = g_dev.versionReg();
    char vs[17];
    memcpy(vs, v, 16);
    vs[16] = 0;
    for (uint8_t i = 0; i < 16; i++) {
      if (vs[i] < 0x20 || vs[i] > 0x7E) vs[i] = '.';
    }
    dsLog("  [T2] LoRa (U2 Ra-01S/SX1268, SPI CS=G%d SCK=G%d MOSI=G%d MISO=G%d"
          " | RST=G%d BUSY=G%d)\n",
          PIN_SX_NSS, PIN_SX_SCLK, PIN_SX_MOSI, PIN_SX_MISO, PIN_SX_RST,
          PIN_SX_BUSY);
    dsLog("       RadioLib 类 = %s（自动探测命中）；版本寄存器 0x0320 = \"%s\"\n",
          g_dev.chipClass(), vs);
    dsLog("       begin(%.1fMHz,BW%.0fk,SF%d,CR4/%d,sync=0x%02X,%ddBm,前导%d) = 0 OK\n",
          (double)RADIO_FREQ_MHZ, (double)RADIO_BW_KHZ, (int)RADIO_SF,
          (int)RADIO_CR, RADIO_SYNC_WORD, (int)RADIO_TX_POWER_DBM,
          (int)RADIO_PREAMBLE_LEN);
    dsLog("       理论空口(10B) = %luus；DIO1 未接线 → 收发走寄存器轮询\n",
          (unsigned long)g_dev.theoreticalAirUs());
    dsLog("#RESULT lora=PASS\n");
  }

  if (!g_t3Ran) {
    dsLog("  [T3] ESP-NOW 尚未检测\n");
  } else if (g_espPass) {
    char pm[18];
    macStr(g_esp.peer(), pm);
    dsLog("  [T3] ESP-NOW 自动连接：%s (rssi=%d dBm, ack=%lu rx=%lu)\n", pm,
          (int)g_esp.peerRssi(), (unsigned long)g_esp.ackOk(),
          (unsigned long)g_esp.rxTotal());
    dsLog("#RESULT espnow=PASS\n");
  } else {
    dsLog("  [T3] ESP-NOW: %s\n", espCodeName(g_espCode));
    for (uint16_t i = 0; i < g_esp.candCount() && i < 5; i++) {
      const EsCandidate *c = g_esp.candidate(i);
      if (!c) break;
      char cm[18];
      macStr(c->mac, cm);
      dsLog("       候选[%u] %s rssi=%d dBm 帧=%u\n", i, cm, (int)c->rssi, c->frames);
    }
    dsLog("#RESULT espnow=FAIL (code=%d)\n", g_espCode);
  }

  printT4();
  printT5();

  dsLog("  LED：快闪%dms=射频FAIL；中速%dms=未连网关；慢闪%dms=已连网关；"
        "每秒一次短亮=光路矫正发射\n",
        LED_FAIL_HALF_MS, LED_SCANNING_HALF_MS, LED_ONLINE_HALF_MS);
  dsLog("#ready 命令 h 帮助；i 报告；x 重启\n");
}

static void printHelp() {
  dsLog("\n[gun-selftest] 串口命令（行末回车）\n");
  dsLog("  h / ?      本帮助\n");
  dsLog("  i          立即打印完整报告\n");
  dsLog("  s          重新扫描 LoRa 网关\n");
  dsLog("  j          立刻补发 JOIN（强制重新注册）\n");
  dsLog("  k<0-19>    手动锁定频点（跳过扫描，如 k7）\n");
  dsLog("  I          暂停/恢复 光路矫正发射（测 LoRa 时想排除干扰就用它）\n");
  dsLog("  b          立刻发一帧光路矫正信号（单帧，用于快速确认）\n");
  dsLog("  c0 / c1 / c2  发射通道：0=近距(G47) 1=远距(G13) 2=两个都发\n");
  dsLog("  x          重启\n");
  dsLog("当前：网关=%s slot=%u 光路发射=%s 通道=%u\n\n",
        g_dev.link().assigned ? "已连接"
                              : (g_dev.link().locked ? "注册中" : "未连接"),
        (unsigned)g_dev.link().slot, g_ir.enabled() ? "开" : "关",
        (unsigned)g_ir.channel());
}

// ---------------------------------------------------------------- LED
static void ledUpdate(uint32_t now) {
  uint32_t half;
  if (!g_dev.link().ready) {
    half = LED_FAIL_HALF_MS;
  } else if (g_dev.link().assigned) {
    half = LED_ONLINE_HALF_MS;
  } else {
    half = LED_SCANNING_HALF_MS;
  }
  bool on = ((now / half) & 1u) == 0;
  // 光路矫正发射瞬间叠加一次短亮（肉眼可见"每秒闪一下"）
  if (g_lastIrMs && (now - g_lastIrMs) < 40) on = true;
  digitalWrite(PIN_LED, on ? LOW : HIGH);   // 低电平点亮
}

// ---------------------------------------------------------------- 命令
static char s_line[24];
static uint8_t s_lineLen = 0;

static void execCmd(const char *cmd) {
  const char c = cmd[0];
  if (!c) return;
  const long arg = atol(cmd + 1);
  switch (c) {
    case 'h': case '?':
      printHelp();
      break;
    case 'i':
      printFullReport();
      break;
    case 's':
      g_dev.requestRescan();
      dsLog("#cmd 重新扫描网关\n");
      break;
    case 'j':
      g_dev.sendJoinNow();
      dsLog("#cmd 补发 JOIN\n");
      break;
    case 'k':
      g_dev.setGridManual((uint8_t)((arg < 0) ? 0 : arg));
      dsLog("#cmd 手动锁定 k=%ld\n", arg);
      break;
    case 'I':
      g_ir.setEnabled(!g_ir.enabled());
      dsLog("#cmd 光路矫正发射 = %s\n", g_ir.enabled() ? "开" : "关");
      break;
    case 'b':
      g_ir.sendBurst();
      g_lastIrMs = g_ir.lastBurstMs();
      dsLog("#cmd 已发一帧光路矫正信号（耗时 %luus）\n",
            (unsigned long)g_ir.lastBurstUs());
      break;
    case 'c':
      g_ir.setChannel((uint8_t)((arg < 0) ? 0 : arg));
      dsLog("#cmd 发射通道 = %u（0=近距 1=远距 2=两个）\n", (unsigned)g_ir.channel());
      break;
    case 'x':
      dsLog("#cmd 重启…\n");
      delay(200);
      ESP.restart();
      break;
    default:
      dsLog("#cmd 未知命令 \"%s\"（h 看帮助）\n", cmd);
      break;
  }
}

static void handleSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (s_lineLen) {
        s_line[s_lineLen] = 0;
        s_lineLen = 0;
        execCmd(s_line);
      }
      continue;
    }
    if (c == 8 || c == 127) { if (s_lineLen) s_lineLen--; continue; }
    if (s_lineLen < sizeof(s_line) - 1) s_line[s_lineLen++] = c;
  }
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);      // 灭

  Serial.begin(SERIAL_BAUD);        // 原生 USB CDC（ARDUINO_USB_CDC_ON_BOOT=1）
  delay(200);
  g_bootMs = millis();

  // 上电三闪：不接串口也能确认代码已启动
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, LOW);  delay(120);
    digitalWrite(PIN_LED, HIGH); delay(120);
  }

  // T2 / T4：射频一次性初始化（T2 只报结果，T4 用同一个实例收发）
  if (g_dev.begin()) {
    g_dev.start();                // T4：自动连网关 + 每秒心跳
  }

  // T5：光路矫正发射（core1 优先级 6，每秒一帧，最低功率）
  g_ir.begin();
  g_ir.start();

  // T3：ESP-NOW 自动连接
  g_t3Ran = true;
  g_esp.begin(ESPNOW_CHANNEL);
  g_espCode = g_esp.run(3000);
  g_espPass = (g_espCode == EspNowAuto::RC_CONNECTED);

  printFullReport();
}

void loop() {
  const uint32_t now = millis();

  // 主机后接入 → 补打一次完整报告（等 300ms 让 CDC 端点就绪，避免丢帧）
  if (!g_hostSeen && (bool)Serial) {
    g_hostSeen = true;
    delay(300);
    printFullReport();
    printHelp();
  }

  // 光路矫正发射时间戳（LED 短亮提示 + 心跳里显示 ir 计数）
  const uint32_t irMs = g_ir.lastBurstMs();
  if (irMs != g_lastIrMs) g_lastIrMs = irMs;

  // 心跳：接入后每 5s 一行；首个周期复述完整报告（枚举窗口的打印可能已丢）
  static uint32_t lastHb = 0;
  if (now - lastHb >= HB_MS) {
    lastHb = now;
    if (g_reportPending) {
      g_reportPending = false;
      printFullReport();
    }
    const DevLink &L = g_dev.link();
    dsLog("#hb t=%lus lora=%s gw=%s k=%u slot=%u N=%u beacon=%lu rssi=%d "
          "hb=%lu(+%lums) ir=%lu\n",
          (unsigned long)(now / 1000), L.ready ? "PASS" : "FAIL",
          L.assigned ? "OK" : (L.locked ? "JOIN" : "--"), (unsigned)L.grid,
          (unsigned)L.slot, (unsigned)L.n, (unsigned long)L.beacons,
          (int)L.beaconRssi, (unsigned long)L.hbTx,
          (unsigned long)(L.lastHbMs ? (now - L.lastHbMs) : 0),
          (unsigned long)g_ir.txCount());
  }

  ledUpdate(now);
  handleSerial();
  delay(2);
}
