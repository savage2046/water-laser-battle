#include <Arduino.h>
#include "config.h"
#include "CurrentSense.h"
#include "PowerSwitch.h"
#include "EspNowLink.h"

CurrentSense sense;
PowerSwitch pwr;
EspNowLink link;

static uint8_t g_fireSeq = 0;     // 开火帧序号（检测板侧）
static uint8_t g_hbCount = 0;
static unsigned long g_lastHb = 0;
static unsigned long g_lastSense = 0;
static bool g_powerRequested = true;  // 枪端最后指令（默认导通）

// ===== 识别回调 → ESP-NOW 上报 =====
static void onFirePulse() {
  link.send(FRAME_FIRE, ++g_fireSeq, 0);
  digitalWrite(PIN_LED, HIGH);   // 每发闪灯（调试/视觉反馈）
  Serial.printf("[fire] #%u\n", g_fireSeq);
}
static void onFireEnd() {
  digitalWrite(PIN_LED, LOW);
}
static void onStall() {
  // 堵转保护：自动断电（防烧电机/电池），上报异常
  pwr.off();
  link.send(FRAME_FAULT, 0x01, 0);
  Serial.println("[stall] auto power-off (overcurrent)");
}

// ===== ESP-NOW 接收：断电/恢复指令 =====
static void handleRx() {
  uint8_t type = 0, d0 = 0;
  while (link.received(type, d0)) {
    if (type == FRAME_POWER) {
      if (d0 == 0x00) {
        pwr.off();
        g_powerRequested = false;
        Serial.println("[pwr] OFF (killed/empty)");
      } else {
        pwr.on();
        g_powerRequested = true;
        Serial.println("[pwr] ON (respawn/reload)");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  pwr.begin();          // 默认导通
  sense.begin();
  static const uint8_t kGunMac[6] = GUN_MAC;
  link.begin(kGunMac);

  // 开机空闲基线自校准（静置 500ms，扣除检测板自身耗电）
  sense.calibrateIdle(500);

  sense.onFirePulse = onFirePulse;
  sense.onFireEnd = onFireEnd;
  sense.onStall = onStall;

  Serial.println("[board] ready, monitoring current");
}

void loop() {
  uint32_t now = millis();

  // 心跳（1Hz，枪端据此判断检测板在线）
  if (now - g_lastHb >= HB_INTERVAL_MS) {
    g_lastHb = now;
    link.send(FRAME_HB, ++g_hbCount, 0);
  }

  // 电流采样 + 识别（~1ms 间隔；INA226 转换周期 ~1.2ms 自然限速）
  if (now - g_lastSense >= SENSE_INTERVAL_MS) {
    g_lastSense = now;
    sense.update(now);
  }

  // ESP-NOW 指令处理
  handleRx();

  delay(1);
}
