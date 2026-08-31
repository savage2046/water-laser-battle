// ============================================================
// current-test：激发事件检测固件（ESP32-S3 + INA226）
//
// 判定规则（射击点）：
//   电流首次 > 1A  → 激发开始（射击点，#FIRE START）
//   电流 < 1A（持续 50ms）→ 激发结束（#FIRE STOP），循环检测
//   负值不做零点校准、忽略（INA226 零点噪声，不参与判定）
//
// 电流平滑（readA → smoothA）：
//   1kHz 采样下点对点抖动 ±0.5~1A，叠加在 30ms 射速谷底上。readA() 对 INA226
//   读数做 SMOOTH_N=5 点滑动平均（低通），压掉 2~3ms 高频抖动；谷底检测与
//   #BLOCK 波形存储统一使用平滑值。
//
// 连发单发检测（detectValleyShot）：
//   连发时扳机持续按住、电流始终 > 1A，每一发表现为电流波动的谷底→上升沿。
//   每收到一个采样：跟踪下降沿，记录谷底最低点；确认谷底并开始上升时（情况B），
//   若峰谷差 ≥ VALLEY_DROP_A 且距上一发 ≥ SHOT_MIN_GAP_MS → 计一发并输出 #SHOT。
//   发数完全由谷底检测确认：n=1 即第一个谷底（#FIRE START 仅标记激发开始，不计数）。
//
// 记录流程（自动循环）：
//   1) WAIT：电流 > 1A（第一次）→ #FIRE START（射击点）+ 开始记录
//   2) RECORD：每 1ms 采样一次（1000Hz），同时检测回落与连发谷底
//      —— 电流 < 1A 持续 50ms → 提前结束本轮（激发停止）
//      —— 满 2000 点（2s）仍不回落 = 长按，强制结束（上限保护）
//   3) 激发结束后一次性返回数据（#BLOCK begin ... #BLOCK end），期间不采样
//   4) 回到 WAIT，等待下一次激发（循环）
// 串口命令见 '?'。详见 README.md。
// ============================================================
#include <Arduino.h>
#include "config.h"
#include "INA226.h"

INA226 ina;

// ---- 记录缓冲（最多 2000 × 1ms = 2s）----
struct Sample { uint16_t tMs; int16_t mA; };   // tMs 相对触发时刻；mA 电流
static Sample g_buf[RECORD_COUNT];

// ---- 状态机 ----
enum Phase { PH_WAIT, PH_RECORD, PH_TX, PH_COOLDOWN };
static Phase g_phase = PH_WAIT;
static uint32_t g_lastUs = 0;      // 轮询（WAIT/COOLDOWN）节拍
static uint32_t g_nextUs = 0;      // 记录（RECORD）下一采样点
static uint32_t g_startMs = 0;     // 本轮激发开始时刻（相对开机）
static uint32_t g_fireStartMs = 0; // 本轮激发开始时刻（用于 #FIRE STOP 时长）
static uint16_t g_idx = 0;         // 已记录样本数
static uint32_t g_cycle = 0;       // 已发送的记录轮次
static uint32_t g_dropSinceMs = 0; // 回落去抖计时
static float g_peakA = 0, g_sumA = 0;

// ---- 连发谷底检测状态（每次激发重置）----
static float    g_vPrevA = 0.0f;      // 上一采样电流
static float    g_vDropFromA = 0.0f;  // 本次下降起点（下降前的峰值）
static float    g_vValleyA = 0.0f;    // 本次下降的谷底最低点
static bool     g_vFalling = false;   // 是否处于下降沿
static uint32_t g_vLastShotMs = 0;    // 上一发时刻（最小间隔）
static uint16_t g_vShots = 0;         // 本轮激发内已检测到的发数
static uint32_t g_vT3 = 0;            // 第三发时刻（稳定射速起点，扣除 n1/n2 后）
static uint32_t g_vTLast = 0;         // 最后一发时刻（稳定射速终点）

// 启动段抑制（方案A）：激发开始后电流从 0 冲上浪涌峰值再回落，此段电流形态
// 是启动瞬态（回弹谷底会被误判成一发，如 n2）。判定：跟踪本轮峰值 g_vSurgePeakA，
// 直到电流回落到 ≤ 峰值×50% 才开启谷底检测（g_vArmed），从源头消灭假谷底。
static float g_vSurgePeakA = 0.0f;   // 本轮启动浪涌峰值（持续跟踪）
static bool  g_vArmed = false;       // 已过启动段（峰值回落过半），允许检测谷底

// ---- 电流平滑：滑动平均（低通）----
// 环形缓冲 + 累加和，O(1)/点。1kHz 采样下压掉 2~3ms 高频抖动，
// 保留 30ms 射速谷底。readA() 统一返回平滑值：谷底检测与波形存储都用平滑数据。
static float g_smBuf[SMOOTH_N];
static uint8_t g_smIdx = 0;
static float g_smSum = 0;
static uint8_t g_smCnt = 0;

static float readA() {
  const float raw = ina.currentA();
  // 移除最旧值（窗口未满时不移除）
  if (g_smCnt >= SMOOTH_N) g_smSum -= g_smBuf[g_smIdx];
  // 写入新值
  g_smBuf[g_smIdx] = raw;
  g_smSum += raw;
  g_smIdx = (g_smIdx + 1) % SMOOTH_N;
  if (g_smCnt < SMOOTH_N) g_smCnt++;
  return g_smSum / g_smCnt;   // 窗口未满时用已有点数平均
}

// ===== 连发单发谷底检测 =====
// 每次采样后调用。确认"谷底 → 开始上升"时打印一发（#SHOT）。
// 判定：总峰谷差（下降起点峰值 − 谷底）≥ VALLEY_DROP_A（0.8A，平滑后数据），
//       且距上一发 ≥ SHOT_MIN_GAP_MS（防单峰被噪声拆成两发）。
//       drop < 0.8A 视为电流抖动，不算一发。
// 启动段抑制：激发开始后电流冲上浪涌峰值，期间不做谷底检测；
//       直到电流从峰值回落到 ≤ 峰值×50% 才允许检测（见 g_vSurgePeakA/g_vArmed），
//       从源头消灭启动浪涌回弹造成的假谷底（如 n2）。
// 仅在激发区间（电流 > 1A）内检测；跌到阈值以下（停止段/负值）重置，
// 避免把"松扳机停机"误判成一发。
static void detectValleyShot(float iA, uint32_t nowMs) {
  if (iA <= TRIG_CURRENT_A) {
    g_vFalling = false;
    g_vPrevA = iA;
    return;
  }

  // 启动段抑制：持续跟踪峰值；电流回落到峰值×50% 以下后武装
  if (iA > g_vSurgePeakA) g_vSurgePeakA = iA;
  if (!g_vArmed) {
    if (g_vSurgePeakA > 0 && iA <= g_vSurgePeakA * 0.5f) g_vArmed = true;
    else {
      g_vFalling = false;      // 启动段内不跟踪谷底
      g_vPrevA = iA;
      return;
    }
  }

  if (iA < g_vPrevA) {
    // 下降沿：首次下降时记住起点峰值，之后持续更新谷底最低点
    if (!g_vFalling) {
      g_vDropFromA = g_vPrevA;
      g_vFalling = true;
    }
    g_vValleyA = iA;
  } else if (iA > g_vPrevA) {
    if (g_vFalling) {
      // 确认谷底（g_vValleyA 为最低点）并开始上升 → 一发
      const float drop = g_vDropFromA - g_vValleyA;   // 总峰谷差
      const bool deepOk = drop >= VALLEY_DROP_A;
      const bool gapOk  = (nowMs - g_vLastShotMs) >= SHOT_MIN_GAP_MS;
      if (deepOk && gapOk) {
        g_vShots++;
        g_vLastShotMs = nowMs;
        g_vTLast = nowMs;              // 最后一发时刻（稳定射速终点）
        if (g_vShots == 3) g_vT3 = nowMs;  // 第三发时刻（稳定射速起点，扣除 n1/n2）
        Serial.printf("#SHOT n=%u t=%lu ms I=%.3f A (drop=%.2f A)\n",
                      (unsigned)g_vShots, (unsigned long)nowMs, (double)iA,
                      (double)drop);
      }
      g_vFalling = false;
      g_vValleyA = iA;               // 谷底已处理，重置为当前点
    }
  }
  // 平台（相等）不动
  g_vPrevA = iA;
}

// ===== 激发开始：首次电流 > 1A（射击点）=====
static void startRecord(uint32_t nowMs, float iA) {
  g_phase = PH_RECORD;
  g_startMs = nowMs;
  g_fireStartMs = nowMs;
  g_idx = 0;
  g_peakA = iA;
  g_sumA = iA;                        // 首个样本计入统计
  g_buf[0].tMs = 0;
  g_buf[0].mA = (int16_t)roundf(iA * 1000.0f);
  g_idx = 1;
  g_nextUs = micros() + RECORD_DT_US;
  digitalWrite(PIN_LED, HIGH);
  // 重置连发谷底检测状态
  g_vPrevA = iA;
  g_vDropFromA = iA;
  g_vValleyA = iA;
  g_vFalling = false;
  g_vLastShotMs = nowMs;  // 上一发时刻暂为激发开始时刻：启动段抑制后
                          // 第一个谷底若距激发开始 < SHOT_MIN_GAP_MS 会被过滤
  g_vShots = 0;   // 发数从 0 开始：每一发都由谷底检测确认（无预置 n1）
  g_vT3 = 0;      // 第三发时刻（稳定射速起点，尚未出现）
  g_vTLast = 0;   // 最后一发时刻（尚未出现）
  // 启动段抑制（方案A）：从激发开始电流起跟踪浪涌峰值，回落过半前不做谷底检测
  g_vSurgePeakA = iA;
  g_vArmed = false;
  // 射击点：第一次电流超过阈值（仅标记激发开始，不计数）
  Serial.printf("#FIRE START t=%lu ms I=%.3f A\n",
                (unsigned long)nowMs, (double)iA);
  Serial.println("#REC start");
}

// ===== 激发结束：一次性返回本轮数据（发送期间不采样/不记录）=====
// stopMs：检测到"电流 < 1A 持续 50ms"的时刻（激发结束时刻）
static void sendBlock(uint32_t stopMs) {
  const uint32_t n = g_idx;                    // 实际记录点数
  const float avg = n ? g_sumA / n : 0.0f;
  const uint32_t dur = stopMs - g_fireStartMs;

  // 稳定射速：扣除前两发（n1 启动第一发、n2 启动后不稳定发），
  // 用 n3 至最后一发的间隔计算：freq = (发数−3) / (tLast − t3)
  // 例：shots=63（n1..n63），稳定段 n3..n63 = 61 发、60 个间隔。
  float freqHz = 0.0f;
  if (g_vShots >= 3 && g_vT3 > 0 && g_vTLast > g_vT3) {
    const uint32_t stableShots = g_vShots - 2;         // n3..nN 的发数
    const uint32_t intervals   = stableShots - 1;      // 间隔数
    freqHz = (float)intervals * 1000.0f / (float)(g_vTLast - g_vT3);
  }

  Serial.printf("#FIRE STOP t=%lu ms dur=%lu ms peak=%.3f A avg=%.3f A shots=%u freq=%.1f Hz n=%lu\n",
                (unsigned long)stopMs, (unsigned long)dur,
                (double)g_peakA, (double)avg, (unsigned)g_vShots,
                (double)freqHz, (unsigned long)n);
  Serial.printf("#BLOCK begin cycle=%lu n=%lu dt=%dus peak=%.3f A avg=%.3f A\n",
                (unsigned long)(++g_cycle), (unsigned long)n, RECORD_DT_US,
                (double)g_peakA, (double)avg);
  for (uint16_t i = 0; i < n; i++) {
    // Serial.printf("%u,%.3f A\n", g_buf[i].tMs, (double)(g_buf[i].mA * 0.001f));

    // Serial.printf(">A:%.3f\n",(double)(g_buf[i].mA * 0.001f));

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
  Serial.println("#  q        查询状态（相位/阈值/轮次）");
  Serial.println("#  v        查询一次 Vbus / 当前电流");
  Serial.println("#  d        读取 INA226 寄存器（诊断）");
  Serial.println("#  ? / h    本帮助");
}

static void printStatus() {
  static const char* ph[] = { "WAIT", "RECORD", "TX", "COOLDOWN" };
  Serial.printf("#status phase=%s trig=%.3f A cycles=%lu\n",
                ph[g_phase], (double)TRIG_CURRENT_A, (unsigned long)g_cycle);
  Serial.printf("#vbus=%.0f mV current=%.3f A\n",
                (double)ina.busVoltageMv(), (double)readA());
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

  // 上电拉高 G10：使电机回路 MOSFET 导通（HIGH=导通），电流才能流过采样电阻
  pinMode(PIN_MOSFET, OUTPUT);
  digitalWrite(PIN_MOSFET, HIGH);
  Serial.println("#mosfet ON (G10 HIGH)");

  Serial.println();
  Serial.println("===== current-test : INA226 current recorder =====");
  const bool ok = ina.begin(PIN_SDA, PIN_SCL, I2C_CLK_HZ);
  if (!ok)
    Serial.println("#ERROR INA226 not detected (check SDA/SCL wiring & addr 0x40)");
  Serial.printf("#shunt=%d mOhm  vbus=%.0f mV\n",
                SHUNT_MOHM, (double)ina.busVoltageMv());

  Serial.printf("#ARMED record=%d x %dus  trig=%.3f A\n",
                RECORD_COUNT, RECORD_DT_US, (double)TRIG_CURRENT_A);
  Serial.println("#type '?' for commands");
}

void loop() {
  handleSerial();

  const uint32_t nowUs = micros();
  const uint32_t nowMs = millis();

  switch (g_phase) {
    case PH_WAIT:
    case PH_COOLDOWN: {
      // 轮询阶段：每 0.5ms 检测一次触发 / 回落（发送阶段不在这里，见 PH_TX）
      if (nowUs - g_lastUs >= POLL_INTERVAL_US) {
        g_lastUs = nowUs;
        const float iA = readA();
        if (g_phase == PH_WAIT) {
          if (iA > TRIG_CURRENT_A) startRecord(nowMs, iA);
        } else {
          if (iA < TRIG_CURRENT_A) {
            if (g_dropSinceMs == 0) g_dropSinceMs = nowMs;
            else if (nowMs - g_dropSinceMs >= DROP_DEBOUNCE_MS) {
              g_phase = PH_WAIT;
              g_dropSinceMs = 0;
              Serial.printf("#ARMED cycle=%lu trig=%.3f A\n",
                            (unsigned long)g_cycle, (double)TRIG_CURRENT_A);
            }
          } else {
            g_dropSinceMs = 0;
          }
        }
      }
      break;
    }

    case PH_RECORD: {
      // 记录阶段：严格每 1ms 采样一次（累加节拍防漂移）
      if (nowUs >= g_nextUs) {
        g_nextUs += RECORD_DT_US;
        const float iA = readA();
        g_buf[g_idx].tMs = (uint16_t)(nowMs - g_startMs);
        g_buf[g_idx].mA = (int16_t)roundf(iA * 1000.0f);
        g_sumA += iA;
        if (iA > g_peakA) g_peakA = iA;
        g_idx++;

        // 连发单发检测：确认谷底并开始上升 → 打印 #SHOT
        detectValleyShot(iA, nowMs);

        // 回落检测：电流 < 1A 持续 50ms → 激发停止，提前结束本轮
        if (iA < TRIG_CURRENT_A) {
          if (g_dropSinceMs == 0) g_dropSinceMs = nowMs;
          else if (nowMs - g_dropSinceMs >= DROP_DEBOUNCE_MS) {
            g_phase = PH_TX;
            sendBlock(nowMs);            // 激发结束：返回已记录的数据
            break;
          }
        } else {
          g_dropSinceMs = 0;
        }

        // 满 2000 点（2s）仍不回落 = 长按，强制结束（上限保护）
        if (g_idx >= RECORD_COUNT) {
          g_phase = PH_TX;
          sendBlock(nowMs);
        }
      }
      break;
    }

    case PH_TX:
      // 发送阶段：不读电流、不记录（sendBlock 已执行完，等待转入 COOLDOWN）
      break;
  }
}
