// ============================================================
// trigger-board-selftest —— triger-sensor 正式板（ESP32-C3）硬件自检固件
//
// 独立于正式 trigger-board 固件，专用于出厂/维修体检：
//   [T1] 电流检测链路（INA226 @0x40 + 2mΩ 分流 + IO4/IO5 I2C）
//   [T2] MOSFET 断电回路步进（IO3→UCC27517→Q1，需要台架负载）
//   [T3] ESP-NOW 连接（与枪端主控 / espnow-verify / 另一块同固件板）
//
// 输出带 #TAG 前缀便于脚本解析；串口不可用（电池供电无 USB）时静默，
// 结论以板载 LED（IO0）指示，代码见 README.md。
// ============================================================
#include <Arduino.h>
#include "config.h"
#include "Trace.h"
#include "Ina226Probe.h"
#include "EspNowProbe.h"

Ina226Probe ina;
Ina226SelfTestResult t1;
EspNowProbe espnow;

// ======================= 板级状态 =======================
static bool g_gateOn = false;                 // IO3 门极：HIGH=导通
static bool g_monActive = false;              // 扳机电流脉冲监控
static bool g_livePrint = false;              // 周期实时电流打印
static bool g_serialSeen = false;             // 串口连接追赶打印

static void gateOn()  { digitalWrite(PIN_MOSFET, HIGH); g_gateOn = true;  DBG("#GATE ON (回路导通)\n"); }
static void gateOff() { digitalWrite(PIN_MOSFET, LOW);  g_gateOn = false; DBG("#GATE OFF (回路断电)\n"); }

// ======================= LED =======================
enum LedPat {
  PAT_TEST = 0, PAT_ALLPASS, PAT_TXONLY, PAT_WAITLINK, PAT_NOPEER, PAT_FAIL
};
static LedPat g_pat = PAT_TEST;
static uint32_t g_ledFlashUntil = 0;          // 脉冲闪灯覆盖（扳机监控）

static void ledUpdate() {
  uint32_t now = millis();
  bool on = false;
  if (now < g_ledFlashUntil) {                 // 脉冲闪灯优先
    on = true;
  } else if (g_pat != PAT_TEST) {
    uint32_t t = now % 4000;
    switch (g_pat) {
      case PAT_ALLPASS:   on = (t % 1000) < 500; break;            // 慢闪 1Hz 50%
      case PAT_TXONLY:    on = (t < 120) || (t >= 200 && t < 320); break;  // 每 4s 双闪
      case PAT_WAITLINK:  on = (t % 2000) < 1000; break;           // 1s亮/1s灭（等待对端）
      case PAT_NOPEER:    on = (t >= 0 && t < 150); break;         // 每 4s 短亮一次
      case PAT_FAIL:      on = true; break;                        // 常亮
      default: break;
    }
  }
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

static void ledFlashMs(uint32_t ms) {
  digitalWrite(PIN_LED, HIGH);
  g_ledFlashUntil = millis() + ms;
}

static void updatePat() {
  if (!t1.run) { g_pat = PAT_TEST; return; }
  if (!t1.pass || !espnow.initOk()) { g_pat = PAT_FAIL; return; }
  switch (espnow.link()) {
    case EspNowProbe::LINK_BIDIR:   g_pat = PAT_ALLPASS;  break;
    case EspNowProbe::LINK_TX_ONLY: g_pat = PAT_TXONLY;   break;
    case EspNowProbe::LINK_WAIT:
      g_pat = (espnow.elapsedMs() < LINK_SETTLE_MS) ? PAT_WAITLINK : PAT_NOPEER;
      break;
    default:                        g_pat = PAT_FAIL;     break;
  }
}

// ======================= 辅助：平均电流采样 =======================
static float avgCurrentMs(uint32_t ms) {
  uint32_t t0 = millis();
  double sum = 0;
  uint32_t n = 0;
  while (millis() - t0 < ms) {
    sum += ina.readCurrentA();
    n++;
    delay(2);
  }
  return n ? (float)(sum / n) : 0.0f;
}

// ======================= T2：MOSFET 回路步进 =======================
// 门极 OFF → 采样 → ON → 采样 → OFF。接台架负载（12V 灯/大功率电阻）时，
// ΔI≥0.2A 证明 门极驱动+Q1 导通/断电 + INA226 电流测量 全链路正常。
static void runMosfetStep() {
  DBG("\n#T2 ===== [2] MOSFET 断电回路步进自检（IO3→Q1，需台架负载）=====\n");
  gateOff();
  delay(20);
  float iOff = avgCurrentMs(T2_OFF_SAMPLE_MS);
  gateOn();
  delay(T2_ON_SETTLE_MS);
  float iOn = avgCurrentMs(T2_ON_SAMPLE_MS);
  gateOff();                                   // 测完恢复断电（安全）
  float delta = iOn - iOff;
  bool pass = fabsf(delta) >= LOAD_STEP_MIN_A;
  bool noLoad = fabsf(delta) < LOAD_STEP_MIN_A && fabsf(iOn) < 0.05f;
  DBG("#T2 i_off=%.4fA i_on=%.4fA delta=%.4fA -> %s\n", (double)iOff, (double)iOn,
      (double)delta, noLoad ? "SKIP" : (pass ? "PASS" : "FAIL"));
  if (noLoad)
    DBG("#T2 reason=未检测到负载电流（SKIP 不影响结论）：请在 XT30 电池/电机回路串接"
        "台架负载（如 12V 车灯或 4.7Ω/10W 电阻）后重跑 '2'；有水弹枪时门极 ON 且扣扳机"
        "也会出现电流，但建议用常通负载避免误射\n");
  else if (!pass)
    DBG("#T2 reason=门极切换未引起电流变化：查 IO3→UCC27517 焊接、Q1(20N03) 焊接/极性、"
        "负载回路接线\n");
}

// ======================= 扳机电流脉冲监控（命令 'c'）======================
// 门极默认置 ON；采样 INA226，扣扳机时按正式固件的阈值/去抖统计脉冲。
static uint8_t  monState = 0;                  // 0=IDLE 1=FIRING 2=STALL
static uint32_t monHiSince = 0, monLoSince = 0, monLastPulse = 0;
static uint32_t monPulseCount = 0, monLastSample = 0;
static float monIdleOffset = 0, monShotPeak = 0;

static void monStart() {
  gateOn();                                    // 监控需要回路导通
  delay(50);
  monIdleOffset = ina.calibrateIdle(300, CS_CAL_SAMPLES);
  monState = 0; monHiSince = monLoSince = monLastPulse = 0;
  monPulseCount = 0; monShotPeak = 0; monLastSample = 0;
  g_monActive = true;
  DBG("#MON 扳机电流监控开始：空闲基线=%.4fA；扣扳机观察 #PULSE，'c'/'q' 退出\n",
      (double)monIdleOffset);
}

static void monStop() {
  g_monActive = false;
  DBG("#MON 监控结束：共 %lu 个脉冲\n", (unsigned long)monPulseCount);
}

static void monUpdate(uint32_t now, float i) {
  bool high = (i - monIdleOffset) > TRIG_CURRENT_A;
  if (monState == 0) {                         // IDLE
    if (high) {
      if (monHiSince == 0) monHiSince = now;
      if (now - monHiSince >= DEBOUNCE_MS) {
        monState = 1; monHiSince = now; monLoSince = 0;
        monLastPulse = now; monShotPeak = fabsf(i);
        monPulseCount++; ledFlashMs(40);
        DBG("#PULSE n=%lu i=%.2fA\n", (unsigned long)monPulseCount, (double)i);
      }
    } else {
      monHiSince = 0;
    }
  } else if (monState == 1) {                  // FIRING
    if (high) {
      monHiSince = now;
      float a = fabsf(i);
      if (a > monShotPeak) monShotPeak = a;
      if (monLoSince != 0 && now - monLoSince >= PULSE_MIN_GAP_MS &&
          now - monLastPulse >= PULSE_MIN_GAP_MS) {
        monLastPulse = now; monShotPeak = a;
        monPulseCount++; ledFlashMs(40);
        DBG("#PULSE n=%lu i=%.2fA\n", (unsigned long)monPulseCount, (double)i);
      }
      monLoSince = 0;
      if (now - monHiSince >= STALL_MS) {
        monState = 2;                          // 堵转（监控模式不自动断电）
        DBG("#MON 警告：持续高电流≥2s（堵转/卡死），监控模式未自动断电\n");
      }
    } else {
      if (monLoSince == 0) monLoSince = now;
      if (now - monLoSince >= END_MS) {
        DBG("#SHOT end peak=%.2fA\n", (double)monShotPeak);
        monState = 0; monHiSince = monLoSince = 0; monShotPeak = 0;
      }
    }
  } else {                                     // STALL：电流回落才恢复
    if (!high || now - monHiSince >= 5000) {
      monState = 0; monHiSince = monLoSince = 0; monShotPeak = 0;
    }
  }
}

// ======================= 汇总打印 =======================
static void printSummary() {
  DBG("\n================ 自检结果汇总 ================\n");
  const char *cur = !t1.run ? "未运行" : (t1.pass ? "PASS" : "FAIL");
  DBG("#RESULT current_detect=%s\n", cur);
  if (t1.run) {
    DBG("#RESULT   i2c_0x40=%s manuf_id=0x%04X die_id=0x%04X cfg=%s "
        "bus=%.2fV cur_mean=%.4fA stdev=%.4fA baseline=%.4fA\n",
        t1.i2cOk ? "OK" : "MISS", t1.manufId, t1.dieId, t1.cfgOk ? "OK" : "FAIL",
        (double)t1.busMeanV, (double)t1.curMeanA, (double)t1.curStdevA,
        (double)t1.idleBaselineA);
  }
  if (espnow.initOk()) {
    const char *st;
    switch (espnow.link()) {
      case EspNowProbe::LINK_BIDIR:   st = "PASS(双向)";  break;
      case EspNowProbe::LINK_TX_ONLY: st = "PARTIAL(仅上行ACK)"; break;
      case EspNowProbe::LINK_WAIT:
        if (espnow.elapsedMs() < LINK_SETTLE_MS)
          st = "等待对端…";
        else if (espnow.everConfirmed)
          st = "掉线(此前已确认；对端可能断电，'r' 复位重测)";
        else
          st = "FAIL(无对端确认)";
        break;
      default:                        st = "FAIL(init)";  break;
    }
    DBG("#RESULT espnow=%s tx_ok=%lu rx=%lu peers=%u\n", st,
        (unsigned long)espnow.txOkTotal, (unsigned long)espnow.rxTotal,
        espnow.peerCount());
    DBG("#RESULT espnow_own_mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
        espnow.mac[0], espnow.mac[1], espnow.mac[2],
        espnow.mac[3], espnow.mac[4], espnow.mac[5]);
  } else {
    DBG("#RESULT espnow=FAIL(init) ch=%d\n", ESPNOW_CHANNEL);
  }
  DBG("=============================================\n");
}

// ======================= 信息打印 =======================
static void printHelp() {
  DBG("#commands:\n"
      "#  a        全部自检重跑（T1 电流 + T2 MOSFET 步进）\n"
      "#  1        重跑 T1（INA226 电流检测）\n"
      "#  2        重跑 T2（MOSFET 步进，需台架负载）\n"
      "#  3 / s    自检结果汇总 / ESP-NOW 状态\n"
      "#  g0 / g1  回路断电 / 导通\n"
      "#  c        扳机电流脉冲监控开关（自动 g1）\n"
      "#  i        周期实时电流打印开关（1Hz）\n"
      "#  r        ESP-NOW 统计清零 + 删除自动配对\n"
      "#  v        详细 RX 帧打印开关\n"
      "#  b        空闲基线重校准\n"
      "#  m        本机信息\n"
      "#  ? / h    帮助\n");
}

static void printBoardInfo() {
  DBG("#BOARD selftest=v%s chip=%s flash=%uKB free=%uKB sdk=%s\n",
      SELFTEST_VERSION, ESP.getChipModel(), ESP.getFlashChipSize() / 1024,
      ESP.getFreeHeap() / 1024, ESP.getSdkVersion());
}

// ======================= 串口命令 =======================
static void runCommand(char *cmd) {
  char c = tolower(cmd[0]);
  switch (c) {
    case '?': case 'h': printHelp(); break;
    case 'a': {
      DBG("#CMD 全自动自检…\n");
      t1 = Ina226SelfTestResult();             // 重置
      ina.begin();
      ina.runSelfTest(t1);
      runMosfetStep();
      updatePat();
      printSummary();
      break;
    }
    case '1':
      t1 = Ina226SelfTestResult();
      ina.begin();
      ina.runSelfTest(t1);
      updatePat();
      break;
    case '2': runMosfetStep(); break;
    case '3': espnow.printStatus(); break;
    case 's': printSummary(); espnow.printStatus(); break;
    case 'g':
      if (cmd[1] == '0') gateOff();
      else if (cmd[1] == '1') gateOn();
      else DBG("#CMD g0=断电 g1=导通\n");
      break;
    case 'c':
      if (g_monActive) monStop(); else monStart();
      break;
    case 'q':
      if (g_monActive) monStop();
      break;
    case 'i':
      g_livePrint = !g_livePrint;
      DBG("#CMD 实时电流打印=%s\n", g_livePrint ? "ON" : "OFF");
      break;
    case 'r': espnow.reset(millis()); updatePat(); break;
    case 'v':
      espnow.verbose = !espnow.verbose;
      DBG("#CMD 详细 RX=%s\n", espnow.verbose ? "ON" : "OFF");
      break;
    case 'b': {
      float b = ina.calibrateIdle(500, 200);
      DBG("#CMD idle_baseline=%.4fA\n", (double)b);
      break;
    }
    case 'm': printBoardInfo(); espnow.printStatus(); break;
    default:
      DBG("#CMD unknown: %s\n", cmd);
      printHelp();
  }
}

static void handleSerial() {
  static char buf[16];
  static uint8_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (len) { buf[len] = '\0'; runCommand(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = ch;
    }
  }
}

// ======================= setup / loop =======================
void setup() {
  Serial.begin(SERIAL_BAUD);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_MOSFET, OUTPUT);
  gateOff();                                   // 自检默认断电（安全）

  DBG("\n===== trigger-board-selftest v%s =====\n", SELFTEST_VERSION);
  printBoardInfo();
  DBG("#note 编译日期：%s %s\n", __DATE__, __TIME__);

  for (int i = 0; i < 3; i++) {                // 上电三闪
    digitalWrite(PIN_LED, HIGH); delay(120);
    digitalWrite(PIN_LED, LOW);  delay(120);
  }
  g_ledFlashUntil = 0;

  // T1：电流检测链路
  ina.begin();
  ina.runSelfTest(t1);

  // T2：MOSFET 步进（无负载时自动 SKIP）
  runMosfetStep();

  // T3：ESP-NOW（后台持续运行，双向确认后 LED 双闪）
  espnow.begin();
  espnow.onPowerFrame = [](bool on) {
    DBG("#POWER 收到对端下行指令：%s\n", on ? "导通" : "断电");
    if (on) gateOn(); else gateOff();
    ledFlashMs(80);
  };
  updatePat();
  printSummary();
  DBG("#ready 键入 '?' 查看命令\n");
}

void loop() {
  uint32_t now = millis();

  // 串口连接追赶打印（USB CDC 后插入/打开监视器时）
  if (!g_serialSeen && (bool)Serial) {
    g_serialSeen = true;
    DBG("\n#serial connected\n");
    printSummary();
    DBG("#ready 键入 '?' 查看命令\n");
  }

  espnow.update(now);

  // 扳机电流脉冲监控：1ms 采样
  if (g_monActive) {
    if (now - monLastSample >= SENSE_INTERVAL_MS) {
      monLastSample = now;
      monUpdate(now, ina.readCurrentA());
    }
  }

  // 周期实时电流打印
  static uint32_t lastLive = 0;
  if (g_livePrint && now - lastLive >= 1000) {
    lastLive = now;
    DBG("#LIVE gate=%d cur=%.4fA bus=%.3fV\n", (int)g_gateOn,
        (double)ina.readCurrentA(), (double)ina.readBusV());
  }

  updatePat();
  ledUpdate();
  handleSerial();
  delay(1);
}
