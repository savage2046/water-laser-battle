#include <Arduino.h>
#include "config.h"
#include "MultiIrRx.h"
#include "RadioLink.h"
#include "TdmaMac.h"
#include "Gps.h"
#include "MotionSensor.h"
#include "Imu9Axis.h"
#include "AudioPlayer.h"
#include "LedStrip.h"

// 标准频点栅格（与网关自动分配一致，见 TdmaProto.h TDMA_STD_*）
static float kChannels[TDMA_CHANNELS];

// 事件序号：16 位自增，服务器据此去重
static uint16_t g_seq = 0;
static uint16_t nextSeq() { return ++g_seq; }

static void tdmaSend(uint8_t type, const uint8_t *payload5, uint8_t flags) {
  tdma.send(type, payload5, flags, nextSeq());
}

// 5B deviceId payload
static void devIdPayload(uint8_t out[5]) {
  const char *id = DEVICE_ID;
  for (int i = 0; i < 5; i++) out[i] = id[i] ? (uint8_t)id[i] : 0;
}

// GPS 位置上报定时
static unsigned long g_lastGpsReport = 0;

// 生命状态（V 帧同步，驱动灯效）
static bool g_inMatch = false;
static bool g_matchEnd = false;
static bool g_alive = true;

// 命中去重：按 (shooter, shotSeq) 精确去重——同一射击帧无论被几路接收头
// 收到只算一次；连发（不同 seq）分别算。环形表 16 条。
struct HitKey { uint16_t shooter; uint8_t seq; };
static HitKey g_dedup[16];
static uint8_t g_dedupIdx = 0;
static bool isDuplicateHit(uint16_t shooter, uint8_t shotSeq) {
  for (uint8_t i = 0; i < 16; i++) {
    if (g_dedup[i].shooter == shooter && g_dedup[i].seq == shotSeq) {
      return true;
    }
  }
  g_dedup[g_dedupIdx].shooter = shooter;
  g_dedup[g_dedupIdx].seq = shotSeq;
  g_dedupIdx = (g_dedupIdx + 1) % 16;
  return false;
}

// 开局复位：清去重表（新局旧帧不误计）
static void resetDedup() {
  for (uint8_t i = 0; i < 16; i++) {
    g_dedup[i].shooter = 0;
    g_dedup[i].seq = 0;
  }
  g_dedupIdx = 0;
}

// 心跳定时（注册由 TDMA MAC 自动 JOIN 处理）
static unsigned long g_lastHb = 0;

// 下行帧处理（TDMA 二进制）：S/E/V 对局状态与生命同步
// 非本机目标丢弃（广播介质，所有设备都能听到）
static void handleTdmaFrame(const TdmaFrame &f) {
  if (f.devIdx != TF_BROADCAST_IDX && f.devIdx != DEV_IDX) return;
  switch (f.type) {
    case TF_START:  // 开局：复位 + 音效
      resetDedup();
      g_inMatch = true;
      g_matchEnd = false;
      g_alive = true;
      audio.play(AudioPlayer::FX_START);
      led.setState(true, true, false);
      Serial.println("[radio] match start");
      break;
    case TF_END:  // 结束
      g_inMatch = false;
      g_matchEnd = true;
      audio.play(AudioPlayer::FX_END);
      led.setState(false, false, true);
      Serial.printf("[radio] match end winner=%d\n",
                    f.payload[0] == 0xFF ? -1 : (int)f.payload[0]);
      break;
    case TF_VITAL: {  // 生命状态同步（确认玩家 ID）
      uint16_t pid = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
      bool wasAlive = g_alive;
      g_alive = f.payload[3] != 0;
      if (g_inMatch && !g_alive && wasAlive) {
        audio.play(AudioPlayer::FX_DEATH);
        led.setState(false, true, false);
      } else if (g_inMatch && g_alive && !wasAlive) {
        audio.play(AudioPlayer::FX_RESPAWN);
        led.setState(true, true, false);
      }
      Serial.printf("[sync] P%u HP%d %s\n", pid, f.payload[2],
                    g_alive ? "ALIVE" : "DEAD");
      break;
    }
    default:
      break;
  }
}

// 位置上报：3 分片（lat/lon/yaw+pitch+roll），连续 3 个自身时隙发出
static void sendPos(long latE6, long lonE6, int16_t yaw, int8_t pitch,
                    int8_t roll) {
  uint8_t p0[5] = { (uint8_t)(latE6 >> 24), (uint8_t)(latE6 >> 16),
                    (uint8_t)(latE6 >> 8), (uint8_t)latE6, 0 };
  tdmaSend(TF_POS, p0, TF_FLAG_FRAG_FIRST | TF_FLAG_FRAG_MORE);
  uint8_t p1[5] = { (uint8_t)(lonE6 >> 24), (uint8_t)(lonE6 >> 16),
                    (uint8_t)(lonE6 >> 8), (uint8_t)lonE6, 0 };
  tdmaSend(TF_POS, p1, TF_FLAG_FRAG_MORE);
  uint8_t p2[5] = { (uint8_t)(yaw >> 8), (uint8_t)yaw, (uint8_t)pitch,
                    (uint8_t)roll, 0 };
  tdmaSend(TF_POS, p2, 0);
}

// ===== GPS 省电状态机 =====
// 状态: 0=定位中(上电等fix) 1=静止待机(断电, 运动唤醒或周期兜底) 2=无fix重试等待
static uint8_t g_gpsState = 0;
static unsigned long g_gpsStateAt = 0;
static unsigned long g_gpsStillSince = 0;  // 上次定位完成时刻（静止冷却）

static void gpsPowerOn() {
  digitalWrite(PIN_GPS_PWR, LOW);  // AO3401 低电平导通 → VCC 上电
}

static void gpsPowerOff() {
  digitalWrite(PIN_GPS_PWR, HIGH); // 断开 VCC（V_BCKP 仍供电保 RTC 星历）
}

static void gpsPowerUpdate() {
#if PIN_GPS_PWR != 0xFF
  switch (g_gpsState) {
    case 0: {  // 定位中
      gps.update();
      if (gps.hasFix()) {
        bool moved = gps.takeMoved();
        if (moved || millis() - g_lastGpsReport >= GPS_REPORT_INTERVAL_MS) {
          g_lastGpsReport = millis();
          int16_t yaw = 0, pitch = 0, roll = 0;
          if (imu.powerOn()) {
            imu.readOrientation(yaw, pitch, roll);
            imu.powerOff();
          }
          sendPos((long)gps.latE6(), (long)gps.lonE6(), yaw, (int8_t)pitch,
                  (int8_t)roll);
        }
        gpsPowerOff();
        g_gpsState = 1;
        g_gpsStateAt = millis();
        g_gpsStillSince = millis();
      } else if (millis() - g_gpsStateAt >= GPS_FIX_TIMEOUT_MS) {
        gpsPowerOff();
        g_gpsState = 2;
        g_gpsStateAt = millis();
      }
      break;
    }
    case 1: {  // 静止待机：GPS 断电（0mA）
      bool moved = motion.takeMotion();
      if (moved && millis() - g_gpsStillSince >= GPS_STILL_SLEEP_MS) {
        gpsPowerOn();
        g_gpsState = 0;
        g_gpsStateAt = millis();
        break;
      }
      if (millis() - g_gpsStateAt >= GPS_POWER_CYCLE_MS) {
        gpsPowerOn();
        g_gpsState = 0;
        g_gpsStateAt = millis();
      }
      break;
    }
    case 2:  // 无 fix 重试等待
      if (millis() - g_gpsStateAt >= GPS_NOFIX_RETRY_MS) {
        gpsPowerOn();
        g_gpsState = 0;
        g_gpsStateAt = millis();
      }
      break;
  }
#else
  // 禁用省电：GPS 常开（原始逻辑）
  gps.update();
  if (gps.hasFix()) {
    bool moved = gps.takeMoved();
    if (moved || millis() - g_lastGpsReport >= GPS_REPORT_INTERVAL_MS) {
      g_lastGpsReport = millis();
      sendPos((long)gps.latE6(), (long)gps.lonE6(), 0, 0, 0);
    }
  }
#endif
}

void setup() {
  Serial.begin(115200);

  const uint8_t irPins[IR_RX_COUNT] = { PIN_IR_RX0, PIN_IR_RX1,
                                        PIN_IR_RX850_0, PIN_IR_RX850_1 };
  irRx.begin(irPins, IR_RX_COUNT);
  radio.begin();
  // 标准频点栅格：470.0 + k×2.0MHz（k=0..19）
  for (int k = 0; k < TDMA_CHANNELS; k++) {
    kChannels[k] = TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ;
  }
  // TDMA MAC：设备模式（头盔 = 附属设备，JOIN 带 TF_FLAG_HELMET）
  uint8_t joinP[5];
  devIdPayload(joinP);
  tdma.setJoinPayload(joinP, TF_FLAG_HELMET);
  tdma.begin(TdmaMac::ROLE_DEVICE, DEV_IDX, radio.getRadio(), kChannels,
             TDMA_CHANNELS, 0, TDMA_MAX_SLOTS);
  gps.begin(PIN_GPS_RX, GPS_BAUD);
  gps.setMovedThresholdM(GPS_REPORT_THRESH_M);
  audio.begin(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT);
  led.begin(PIN_LED, LED_COUNT);
  led.setState(true, false, false);  // 待机青色
  motion.begin(PIN_MOTION_SDA, PIN_MOTION_SCL, PIN_MOTION_INT);
  imu.begin(PIN_IMU_SDA, PIN_IMU_SCL, PIN_IMU_PWR);

#if PIN_GPS_PWR != 0xFF
  pinMode(PIN_GPS_PWR, OUTPUT);
  digitalWrite(PIN_GPS_PWR, LOW);  // 上电
  g_gpsState = 0;
  g_gpsStateAt = millis();
#endif

  Serial.println("[helmet] boot, waiting for TDMA assign");
}

void loop() {
  // 1) 心跳：已分配后每 10s（网关维护活跃设备表）
  if (tdma.assigned() && millis() - g_lastHb > HEARTBEAT_MS) {
    g_lastHb = millis();
    tdmaSend(TF_HB, NULL, 0);
  }

  // 2) 红外接收 → 解码出射手 ID/武器/序号/通道 → 上报 T 帧（头盔被命中）
  LaserFrame hit;
  if (irRx.poll(hit)) {
    if (!isDuplicateHit(hit.playerId, hit.shotSeq)) {
      uint8_t p[5];
      p[0] = (uint8_t)(hit.playerId >> 8);
      p[1] = (uint8_t)(hit.playerId & 0xFF);
      p[2] = hit.weaponId;
      p[3] = hit.shotSeq;
      p[4] = hit.channel;
      tdmaSend(TF_TAG, p, 0);
      audio.play(AudioPlayer::FX_HIT);
      led.flashHit();
      Serial.printf("[hit] shooter=%u weapon=%u seq=%u ch=%u\n", hit.playerId,
                    hit.weaponId, hit.shotSeq, hit.channel);
    }
  }

  // 2.5) 下行：对局状态/生命同步（S/E/V）
  TdmaFrame f;
  while (tdma.pollDownlink(f)) {
    handleTdmaFrame(f);
  }

  // 3) GPS 省电状态机：定位 → 上报 → 断电休眠 → 周期唤醒（热启动）
  gpsPowerUpdate();

  // 4) 音频 DMA 喂数 + 灯效时序
  audio.update();
  led.update();

  delay(2);
}
