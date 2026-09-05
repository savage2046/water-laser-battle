#include <Arduino.h>
#include "config.h"
#include "CurrentSense.h"
#include "PowerSwitch.h"
#include "EspNowLink.h"

// ============================================================
// triger-sensor.eprj2 正式板检测板主程序（ESP32-C3-WROOM-02-N4）
// 电流检测扳机/连发 → ESP-NOW 逐发上报枪端
// MOSFET 物理断电（防作弊）：枪端指令 / 堵转自保护
// ============================================================
CurrentSense sense;
PowerSwitch pwr;
EspNowLink espLink;

static uint8_t g_fireSeq = 0;   // 开火帧序号（检测板侧）
static uint8_t g_hbCount = 0;
static unsigned long g_lastHb = 0;
static unsigned long g_lastSense = 0;

// ===== 识别回调 → ESP-NOW 上报 =====
static void onFirePulse() {
  espLink.send(FRAME_FIRE, ++g_fireSeq, 0);
  digitalWrite(PIN_LED, HIGH);   // 每发闪灯（视觉反馈）
  Serial.printf("[fire] #%u\n", g_fireSeq);
}
static void onFireEnd() {
  digitalWrite(PIN_LED, LOW);
}
static void onStall() {
  // 堵转保护：自动断电（防烧电机/电池），上报异常
  pwr.off();
  espLink.send(FRAME_FAULT, 0x01, 0);
  Serial.println("[stall] auto power-off (overcurrent)");
}

// ===== ESP-NOW 接收：断电/恢复指令 =====
static void handleRx() {
  uint8_t type = 0, d0 = 0;
  while (espLink.received(type, d0)) {
    if (type == FRAME_POWER) {
      if (d0 == 0x00) {
        pwr.off();
        Serial.println("[pwr] OFF (killed/empty)");
      } else {
        pwr.on();
        Serial.println("[pwr] ON (respawn/reload)");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);            // 等待串口稳定（原生 USB-C CDC）

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  pwr.begin();           // 默认导通
  sense.begin();

  static const uint8_t kGunMac[6] = GUN_MAC;
  espLink.begin(kGunMac);
  espLink.printLocalMac();  // 打印本机 MAC，与枪端 TRIGGER_BOARD_MAC 配对

  // 开机空闲基线自校准（静置 500ms，扣除检测板自身耗电）
  sense.calibrateIdle(500);

  sense.onFirePulse = onFirePulse;
  sense.onFireEnd = onFireEnd;
  sense.onStall = onStall;

  Serial.println("[board] triger-sensor esp32c3 ready, monitoring current");
}

void loop() {
  uint32_t now = millis();

  // 心跳（1Hz，枪端据此判断检测板在线）
  if (now - g_lastHb >= HB_INTERVAL_MS) {
    g_lastHb = now;
    espLink.send(FRAME_HB, ++g_hbCount, 0);
  }

  // 电流采样 + 识别（INA226 转换周期 ~1.18ms 自然限速）
  if (now - g_lastSense >= SENSE_INTERVAL_MS) {
    g_lastSense = now;
    sense.update(now);
  }

  // ESP-NOW 指令处理
  handleRx();

  delay(1);
}
