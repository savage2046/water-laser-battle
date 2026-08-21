// ============================================================
// current-test：电流测试专用固件（ESP32-S3 + INA226）
//
// 记录流程（自动循环）：
//   1) 开机测基础电流（空闲均值，电机断电时进行；'c' 可重测）
//   2) 电流 > max(2×基础电流, 1A) → 开始记录
//   3) 每 10ms 记录一次，连续 100 次（1s 窗口），期间 LED 点亮
//   4) 记录满 100 次后一次性返回全部数据（#BLOCK begin ... #BLOCK end）
//      —— 发送阶段不做任何电流采样/记录，避免数据超载
//   5) 电流回落到 1A 以下（持续 50ms）→ 重新武装，进入下一轮
// 串口命令见 '?'。详见 README.md。
// ============================================================
#include <Arduino.h>
#include "config.h"
#include "INA226.h"

INA226 ina;

// ---- 基础电流 / 触发阈值 ----
static float g_baseA = 0.0f;   // 基础电流（空闲均值，开机测一次，'c' 重测）
static float g_trigA = TRIG_CURRENT_A;  // 触发阈值 = max(2×基础电流, 1A)

// ---- 记录缓冲（100 × 10ms = 1s 窗口）----
struct Sample { uint16_t tMs; int16_t mA; };   // tMs 相对触发时刻；mA 电流
static Sample g_buf[RECORD_COUNT];

// ---- 状态机 ----
enum Phase { PH_WAIT, PH_RECORD, PH_TX, PH_COOLDOWN };
static Phase g_phase = PH_WAIT;
static uint32_t g_lastUs = 0;      // 轮询（WAIT/COOLDOWN）节拍
static uint32_t g_nextUs = 0;      // 记录（RECORD）下一采样点
static uint32_t g_startMs = 0;     // 本轮记录起始时刻
static uint16_t g_idx = 0;         // 已记录样本数
static uint32_t g_cycle = 0;       // 已发送的记录轮次
static uint32_t g_dropSinceMs = 0; // 回落去抖计时
static float g_peakA = 0, g_sumA = 0;

static float readA() { return ina.currentA(); }

// ===== 基础电流测量（静置 0.5s 取均值；须在电机断电时执行）=====
static void measureBaseline() {
  Serial.println("#CAL measuring baseline (0.5s, 保持电机断电)...");
  float sum = 0;
  uint32_t n = 0;
  const unsigned long t0 = millis();
  while (millis() - t0 < 500) { sum += readA(); n++; delay(2); }
  g_baseA = (n && sum / n > 0) ? sum / n : 0;
  const float twice = 2.0f * g_baseA;
  g_trigA = (twice > TRIG_CURRENT_A) ? twice : TRIG_CURRENT_A;
  Serial.printf("#CAL done base=%.1f mA trig=%.0f mA (max(2x base, %d mA))\n",
                (double)(g_baseA * 1000.0f), (double)(g_trigA * 1000.0f),
                (int)(TRIG_CURRENT_A * 1000.0f));
}

// ===== 触发：开始一轮记录（首个样本立即记录）=====
static void startRecord(uint32_t nowMs, float iA) {
  g_phase = PH_RECORD;
  g_startMs = nowMs;
  g_idx = 0;
  g_peakA = iA;
  g_sumA = iA;                        // 首个样本计入统计
  g_buf[0].tMs = 0;
  g_buf[0].mA = (int16_t)roundf(iA * 1000.0f);
  g_idx = 1;
  g_nextUs = micros() + RECORD_DT_MS * 1000UL;
  digitalWrite(PIN_LED, HIGH);
  Serial.println("#REC start");
}

// ===== 一次性返回本轮 100 个数据（发送期间不采样/不记录）=====
static void sendBlock() {
  const float avg = g_sumA / RECORD_COUNT;
  Serial.printf("#BLOCK begin cycle=%lu n=%d dt=%dms peak=%.1f mA avg=%.1f mA\n",
                (unsigned long)(++g_cycle), RECORD_COUNT, RECORD_DT_MS,
                (double)(g_peakA * 1000.0f), (double)(avg * 1000.0f));
  for (uint16_t i = 0; i < RECORD_COUNT; i++) {
    Serial.printf("%u,%d\n", g_buf[i].tMs, g_buf[i].mA);
  }
  Serial.println("#BLOCK end");
  digitalWrite(PIN_LED, LOW);
  g_phase = PH_COOLDOWN;
  g_dropSinceMs = 0;
  Serial.println("#WAIT_DROP current < 1A ...");
}

// ===== 串口命令 =====
static void printHelp() {
  Serial.println("#commands:");
  Serial.println("#  c        重测基础电流（电机断电时执行）");
  Serial.println("#  q        查询状态（相位/基础电流/阈值/轮次）");
  Serial.println("#  v        查询一次 Vbus / 当前电流");
  Serial.println("#  d        读取 INA226 寄存器（诊断）");
  Serial.println("#  ? / h    本帮助");
}

static void printStatus() {
  static const char* ph[] = { "WAIT", "RECORD", "TX", "COOLDOWN" };
  Serial.printf("#status phase=%s base=%.1f mA trig=%.0f mA cycles=%lu\n",
                ph[g_phase], (double)(g_baseA * 1000.0f),
                (double)(g_trigA * 1000.0f), (unsigned long)g_cycle);
  Serial.printf("#vbus=%.0f mV current=%.1f mA\n",
                (double)ina.busVoltageMv(), (double)(readA() * 1000.0f));
}

static void dumpRegs() {
  Serial.printf("#regs CONFIG=0x%04X SHUNT=0x%04X BUS=0x%04X CAL=0x%04X MFG=0x%04X DIE=0x%04X\n",
                ina.readReg(0x00), ina.readReg(0x01), ina.readReg(0x02),
                ina.readReg(0x05), ina.readReg(0xFE), ina.readReg(0xFF));
}

static void runCommand(char* cmd) {
  const char c = tolower(cmd[0]);
  switch (c) {
    case '?': case 'h': printHelp(); break;
    case 'c':
      if (g_phase == PH_WAIT || g_phase == PH_COOLDOWN) measureBaseline();
      else Serial.println("#busy: 记录/发送中，稍后再试");
      break;
    case 'v': printStatus(); break;
    case 'q': printStatus(); break;
    case 'd': dumpRegs(); break;
    default:
      Serial.printf("#unknown command: %s\n", cmd);
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

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.println();
  Serial.println("===== current-test : INA226 current recorder =====");
  const bool ok = ina.begin(PIN_SDA, PIN_SCL, I2C_CLK_HZ);
  if (!ok)
    Serial.println("#ERROR INA226 not detected (check SDA/SCL wiring & addr 0x40)");
  Serial.printf("#shunt=%d mOhm  vbus=%.0f mV\n",
                SHUNT_MOHM, (double)ina.busVoltageMv());

  measureBaseline();
  Serial.printf("#ARMED record=%d x %dms  trig=%.0f mA\n",
                RECORD_COUNT, RECORD_DT_MS, (double)(g_trigA * 1000.0f));
  Serial.println("#type '?' for commands");
}

void loop() {
  handleSerial();

  const uint32_t nowUs = micros();
  const uint32_t nowMs = millis();

  switch (g_phase) {
    case PH_WAIT:
    case PH_COOLDOWN: {
      // 轮询阶段：每 2ms 检测一次触发 / 回落（发送阶段不在这里，见 PH_TX）
      if (nowUs - g_lastUs >= POLL_INTERVAL_US) {
        g_lastUs = nowUs;
        const float iA = readA();
        if (g_phase == PH_WAIT) {
          if (iA > g_trigA) startRecord(nowMs, iA);
        } else {
          if (iA < TRIG_CURRENT_A) {
            if (g_dropSinceMs == 0) g_dropSinceMs = nowMs;
            else if (nowMs - g_dropSinceMs >= DROP_DEBOUNCE_MS) {
              g_phase = PH_WAIT;
              g_dropSinceMs = 0;
              Serial.printf("#ARMED cycle=%lu base=%.1f mA trig=%.0f mA\n",
                            (unsigned long)g_cycle, (double)(g_baseA * 1000.0f),
                            (double)(g_trigA * 1000.0f));
            }
          } else {
            g_dropSinceMs = 0;
          }
        }
      }
      break;
    }

    case PH_RECORD: {
      // 记录阶段：严格每 10ms 采样一次（累加节拍防漂移）
      if (nowUs >= g_nextUs) {
        g_nextUs += RECORD_DT_MS * 1000UL;
        const float iA = readA();
        g_buf[g_idx].tMs = (uint16_t)(nowMs - g_startMs);
        g_buf[g_idx].mA = (int16_t)roundf(iA * 1000.0f);
        g_sumA += iA;
        if (iA > g_peakA) g_peakA = iA;
        if (++g_idx >= RECORD_COUNT) {
          g_phase = PH_TX;             // 进入发送阶段后不再采样
          sendBlock();                 // 一次性返回全部数据
        }
      }
      break;
    }

    case PH_TX:
      // 发送阶段：不读电流、不记录（sendBlock 已执行完，等待转入 COOLDOWN）
      break;
  }
}
