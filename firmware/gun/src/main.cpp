#include <Arduino.h>
#include <esp_system.h>   // esp_random() 硬件真随机
#include "config.h"
#include "LaserCodec.h"
#include "GunState.h"
#include "RadioLink.h"
#include "TdmaMac.h"
#include "GunEspNow.h"
#include "Display.h"
#include "LogBuf.h"
#include "AudioPlayer.h"
#include "LedStrip.h"

// 标准频点栅格（与网关自动分配一致，见 TdmaProto.h TDMA_STD_*）
static float kChannels[TDMA_CHANNELS];

// ESP-NOW 链路全局实例（枪端 ↔ 检测板；GunEspNow::_self 在 begin() 中指向它）
GunEspNow gunEspNow;

// 事件序号：16 位自增，多网关重复上报时服务器据此去重
static uint16_t g_seq = 0;
static uint16_t nextSeq() { return ++g_seq; }

// TDMA 上行发送（排队，自身时隙发射）
static void tdmaSend(uint8_t type, const uint8_t *payload5, uint8_t flags) {
  tdma.send(type, payload5, flags, nextSeq());
}

// 5B deviceId payload（"G0001" → 5 ASCII 字节）
static void devIdPayload(uint8_t out[5]) {
  const char *id = DEVICE_ID;
  for (int i = 0; i < 5; i++) out[i] = id[i] ? (uint8_t)id[i] : 0;
}

// H 帧：payload = {shooter(2B BE), shotSeq, weapon<<4|channel, hp}
static void sendHit(uint16_t shooter, uint8_t weapon, uint8_t shotSeq,
                    uint8_t channel, uint8_t hp) {
  uint8_t p[5];
  p[0] = (uint8_t)(shooter >> 8);
  p[1] = (uint8_t)(shooter & 0xFF);
  p[2] = shotSeq;
  p[3] = (uint8_t)((weapon << 4) | (channel & 0x0F));
  p[4] = hp;
  tdmaSend(TF_HIT, p, 0);
}

// 阵亡上报：延迟 30ms 双帧窗口统计光强（同 shotSeq 第二帧 → 强光）
struct PendingDeath {
  bool valid = false;
  uint16_t shooter = 0;
  uint8_t weapon = 0;
  uint8_t shotSeq = 0;
  uint8_t channel = 0;
  uint8_t frames = 1;
  unsigned long at = 0;
};
static PendingDeath g_pendDeath;

static void queueDeath(uint16_t shooter, uint8_t weapon, uint8_t shotSeq,
                       uint8_t channel) {
  g_pendDeath.valid = true;
  g_pendDeath.shooter = shooter;
  g_pendDeath.weapon = weapon;
  g_pendDeath.shotSeq = shotSeq;
  g_pendDeath.channel = channel;
  g_pendDeath.frames = 1;
  g_pendDeath.at = millis();
}

static void deathUpdate() {
  if (!g_pendDeath.valid) return;
  if (millis() - g_pendDeath.at < DEATH_DELAY_MS) return;
  uint8_t energy = g_pendDeath.frames >= 2 ? ENERGY_STRONG : ENERGY_WEAK;
  uint8_t p[5];
  p[0] = (uint8_t)(g_pendDeath.shooter >> 8);
  p[1] = (uint8_t)(g_pendDeath.shooter & 0xFF);
  p[2] = g_pendDeath.shotSeq;
  p[3] = g_pendDeath.channel;
  p[4] = energy;
  tdmaSend(TF_DEATH, p, 0);
  logBuf.log("D,%lu,%u", millis() / 1000, g_pendDeath.shooter);
  g_pendDeath.valid = false;
}

// 扳机消抖
static bool g_triggerWas = false;
static unsigned long g_triggerDebounceAt = 0;
static bool readTrigger() {
  bool now = digitalRead(PIN_TRIGGER) == LOW;  // 按下接地
  if (now != g_triggerWas) {
    g_triggerDebounceAt = millis();
    g_triggerWas = now;
  }
  if (millis() - g_triggerDebounceAt < 20) return g_triggerWas == false && now;
  return now;
}

// 菜单 4 键消抖扫描（上/下/确认/取消，按下接地，上升沿触发一次）
struct BtnState {
  bool was;
  unsigned long debounceAt;
};
static BtnState g_btn[4];
static bool scanBtn(uint8_t pin, BtnState &s, unsigned long debounceMs) {
  bool now = digitalRead(pin) == LOW;
  if (now != s.was) {
    s.debounceAt = millis();
    s.was = now;
  }
  if (millis() - s.debounceAt < debounceMs) return false;
  return s.was == false && now;  // 刚按下
}
static void scanMenuButtons() {
  if (scanBtn(PIN_BTN_UP, g_btn[0], 30)) menu.onUp();
  if (scanBtn(PIN_BTN_DOWN, g_btn[1], 30)) menu.onDown();
  if (scanBtn(PIN_BTN_OK, g_btn[2], 30)) menu.onOk();
  if (scanBtn(PIN_BTN_CANCEL, g_btn[3], 30)) menu.onCancel();
}

// 已注册标志（收到 W welcome）与心跳/重连定时
static bool g_registered = false;
static unsigned long g_lastHb = 0;
static unsigned long g_lastJoin = 0;
static unsigned long g_lastFire = 0;  // F 帧合并（1s 粒度，保护上行容量）

// 无改装联动：枪电源开关状态跟踪。
// 通电条件 = 存活 && 有弹 && 未锁（开局/恢复中）；阵亡/空弹/暂停/结束/未开局 → 断电
static bool g_powerOn = false;  // 初始 false：不发指令（检测板默认导通）
static void gunPowerUpdate() {
  bool need = gun.isAlive() && gun.ammo() > 0 && !gun.isLocked();
  if (need != g_powerOn) {
    g_powerOn = need;
    gunEspNow.sendPower(need);
    Serial.printf("[power] gun %s\n", need ? "ON" : "OFF");
  }
}

// 日志上传：定时器
static unsigned long g_lastLogUpload = 0;
static bool g_logUploading = false;
static uint16_t g_logPart = 0, g_logTotal = 0;

// 上传挂起日志（全部发完后清空缓冲；T1 每片 3B，分片数变多）
static void uploadLogs() {
  if (!g_registered || !logBuf.pending()) {
    g_logUploading = false;
    return;
  }
  if (!g_logUploading) {
    g_logUploading = true;
    g_logPart = 0;
  }
  char payload[LOG_PAYLOAD_MAX + 1];
  uint16_t part = g_logPart, total = 0;
  if (logBuf.nextPayload(payload, LOG_PAYLOAD_MAX, part, total)) {
    uint8_t p[5] = { (uint8_t)part, (uint8_t)total, 0, 0, 0 };
    size_t n = strlen(payload);
    if (n > 3) n = 3;
    memcpy(p + 2, payload, n);
    tdmaSend(TF_LOG, p, 0);
    g_logPart++;
    g_logTotal = total;
    if (g_logPart >= total) {
      logBuf.clear();
      g_logUploading = false;
      Serial.printf("[log] uploaded %u parts\n", g_logTotal);
    }
  } else {
    logBuf.clear();
    g_logUploading = false;
  }
}

// 声光反馈：按事件触发音效 + 灯效（事件变化才触发一次）
static GunState::Event g_lastFeedback = GunState::EV_MATCH_START;
static void handleFeedback() {
  GunState::Event ev = gun.lastEvent();
  if (ev == g_lastFeedback) return;
  g_lastFeedback = ev;
  switch (ev) {
    case GunState::EV_HIT:
      audio.play(AudioPlayer::FX_HIT);
      led.flashHit();
      break;
    case GunState::EV_DEATH:
      audio.play(AudioPlayer::FX_DEATH);
      led.setState(false, true, false);  // 阵亡红
      break;
    case GunState::EV_RESPAWN:
      audio.play(AudioPlayer::FX_RESPAWN);
      break;
    case GunState::EV_RELOAD:
      audio.play(AudioPlayer::FX_CLICK);
      break;
    default:
      break;
  }
}

// ===== 下行帧处理（TDMA 二进制）=====

// W welcome 分片重组（3 片，连续 3 个超帧广播窗送达）
static uint8_t g_wFrag[3][5];
static uint8_t g_wPart = 0;
static void welcomeFrag(const TdmaFrame &f) {
  if (f.flags & TF_FLAG_FRAG_FIRST) g_wPart = 0;
  if (g_wPart < 3) {
    memcpy(g_wFrag[g_wPart], f.payload, 5);
    g_wPart++;
  }
  if (f.flags & TF_FLAG_FRAG_MORE) return;
  if (g_wPart < 3) {
    Serial.printf("[radio] W frag lost (%u/3)\n", g_wPart);
    g_wPart = 0;
    return;  // 缺片丢弃；设备会重发 J 触发服务器重发 welcome
  }
  const uint8_t *p0 = g_wFrag[0], *p1 = g_wFrag[1], *p2 = g_wFrag[2];
  uint16_t pid = (uint16_t)((p0[0] << 8) | p0[1]);
  uint16_t reloadMs = (uint16_t)((p1[0] << 8) | p1[1]);
  uint16_t respawnMs = (uint16_t)((p1[2] << 8) | p1[3]);
  int16_t team = (int8_t)p1[4];
  uint16_t friends[8];
  uint8_t fc = 0;
  uint32_t fb = ((uint32_t)p2[2] << 16) | ((uint32_t)p2[3] << 8) | p2[4];
  for (int i = 0; i < 24 && fc < 8; i++) {
    if (fb & (1u << i)) friends[fc++] = (uint16_t)i;  // 友军 devIdx（T1 假定 playerId=devIdx）
  }
  gun.applyConfig(pid, team, friends, fc, p0[2], p0[3], p0[4], reloadMs,
                  respawnMs, p2[0]);
  laser.setPowerLevel(p2[1]);
  g_registered = true;
  Serial.printf("[radio] welcome pid=%u team=%d hp=%u power=%u\n", pid, team,
                p0[2], p2[1]);
  logBuf.log("W,%lu,%u,%d", millis() / 1000, pid, team);
}

static void handleTdmaFrame(const TdmaFrame &f) {
  // 目标寻址：广播或发给本设备
  if (f.devIdx != TF_BROADCAST_IDX && f.devIdx != DEV_IDX) return;
  switch (f.type) {
    case TF_WELCOME:
      welcomeFrag(f);
      break;
    case TF_START:
      gun.resetForMatch();
      logBuf.log("S,%lu", millis() / 1000);
      audio.play(AudioPlayer::FX_START);
      led.setState(true, true, false);
      break;
    case TF_END:
      gun.endMatch(f.payload[0] == 0xFF ? -1 : (int)f.payload[0]);
      logBuf.log("E,%lu,%d", millis() / 1000,
                 f.payload[0] == 0xFF ? -1 : (int)f.payload[0]);
      g_lastLogUpload = 0;  // 对局结束：尽快上传
      audio.play(AudioPlayer::FX_END);
      led.setState(false, false, true);
      break;
    case TF_RESPAWN:
      gun.respawn();
      logBuf.log("R,%lu", millis() / 1000);
      audio.play(AudioPlayer::FX_RESPAWN);
      led.setState(true, true, false);
      break;
    case TF_PAUSE:
      gun.pause();
      break;
    case TF_RESUME:
      gun.resume();
      break;
    case TF_XHIT: {  // 外部命中（头盔转发）：扣血 + 上报 H
      uint16_t shooter = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
      uint8_t weapon = f.payload[2];
      uint8_t shotSeq = f.payload[3];
      uint8_t channel = f.payload[4];
      bool killed = false;
      if (gun.applyExternalHit(shooter, weapon, shotSeq, killed)) {
        sendHit(shooter, weapon, shotSeq, channel, gun.hp());
        logBuf.log("X,%lu,%u", millis() / 1000, shooter);
        if (killed) queueDeath(shooter, weapon, shotSeq, channel);
      }
      break;
    }
    case TF_VITAL: {  // 生命状态同步（确认玩家 ID，不覆盖本地血量权威）
      uint16_t pid = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
      Serial.printf("[sync] P%u HP%d %s\n", pid, f.payload[2],
                    f.payload[3] ? "ALIVE" : "DEAD");
      break;
    }
    default:
      break;
  }
}

// 电机联动定时（非阻塞：开火时启动，MOTOR_ON_MS 后自动停，与红外发射并行）
static bool g_motorOn = false;
static unsigned long g_motorUntil = 0;
static void motorStart() {
  digitalWrite(PIN_MOTOR, HIGH);
  g_motorOn = true;
  g_motorUntil = millis() + MOTOR_ON_MS;
}
static void motorUpdate() {
  if (g_motorOn && millis() >= g_motorUntil) {
    digitalWrite(PIN_MOTOR, LOW);
    g_motorOn = false;
  }
}

// ===== 户外强光自适应：环境光采样 → 自动升远档 =====
static uint8_t g_lightLevel = 0;   // 0低 1中 2高 3极强
static unsigned long g_lastLight = 0;

// 环境光等级名称（Display.cpp 引用；OLED 状态行 LIGHT:xx）
const char *lightName() {
  switch (g_lightLevel) {
    case 2: return "EXTREME";
    case 1: return "HIGH";
    default: return "LOW";
  }
}
static void lightUpdate() {
#if PIN_LIGHT_SENSE != 0xFF
  if (millis() - g_lastLight < LIGHT_SAMPLE_MS) return;
  g_lastLight = millis();
  int v = analogRead(PIN_LIGHT_SENSE);
  g_lightLevel = (v > LIGHT_HIGH_ADC) ? 2 : (v > LIGHT_HIGH_ADC / 2 ? 1 : 0);
#if LIGHT_AUTO_POWER
  if (g_lightLevel >= 2) {
    laser.setPowerLevel(2);  // 强光自动升远档，补偿 TSOP AGC 压缩
  }
#endif
#endif
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIGGER, INPUT_PULLUP);
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);

  laser.begin(PIN_IR_TX, PIN_IR_TX_850, PIN_IR_RX, PIN_IR_RX_850,
              PIN_IR_POWER, PIN_IR_PWR_850_A, PIN_IR_PWR_850_B);
  radio.begin();
  // 标准频点栅格：470.0 + k×2.0MHz（k=0..19）
  for (int k = 0; k < TDMA_CHANNELS; k++) {
    kChannels[k] = TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ;
  }
#if GUN_ESPNOW_ENABLE
  // 无改装联动：与检测板配对（检测板检测扳机电流 → ESP-NOW 开火事件）
  static const uint8_t kBoardMac[6] = TRIGGER_BOARD_MAC;
  gunEspNow.begin(kBoardMac);
#endif
  // TDMA MAC：设备模式，开机负载均衡扫描 + 注册时隙 JOIN
  uint8_t joinP[5];
  devIdPayload(joinP);
  tdma.setJoinPayload(joinP, 0);
  tdma.begin(TdmaMac::ROLE_DEVICE, DEV_IDX, radio.getRadio(), kChannels,
             TDMA_CHANNELS, 0, TDMA_MAX_SLOTS);
  gun.begin();
  display.begin(PIN_OLED_SDA, PIN_OLED_SCL, OLED_ADDR);
  logBuf.begin();
  menu.begin();
  audio.begin(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT);
  led.begin(PIN_LED_DATA, 1);
  led.setState(false, false, false);  // 待机青色
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP);
  pinMode(PIN_BTN_CANCEL, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) {
    g_btn[i].was = digitalRead(i == 0 ? PIN_BTN_UP
                           : i == 1 ? PIN_BTN_DOWN
                           : i == 2 ? PIN_BTN_OK : PIN_BTN_CANCEL) == LOW;
    g_btn[i].debounceAt = 0;
  }
  laser.setPowerLevel(DEFAULT_POWER_LEVEL);

  Serial.println("[gun] boot, waiting for TDMA beacon + assign");
  logBuf.log("B,0");  // 开机日志（相对秒 0）
}

void loop() {
  // 1) 已分配但未收到 welcome：每 5s 走自身时隙重发 J（触发服务器重发 W）
  if (tdma.assigned() && !g_registered &&
      millis() - g_lastJoin > REJOIN_MS) {
    g_lastJoin = millis();
    uint8_t joinP[5];
    devIdPayload(joinP);
    tdma.send(TF_JOIN, joinP, 0, 0);
  }

  // 2) 已分配后：心跳每 10s（网关据此维护活跃设备表）
  if (tdma.assigned() && millis() - g_lastHb > HEARTBEAT_MS) {
    g_lastHb = millis();
    tdmaSend(TF_HB, NULL, 0);
  }

  // 3) 扳机 → 开火（双载波红外发射 38/56kHz + 电机并行联动 + F 帧）
  //    无改装联动：扳机源 = 本地微动（改装/校准）或检测板 ESP-NOW 开火事件
  bool trig = readTrigger() || gunEspNow.takeFire();
  if (menu.calibrating()) {
    if (trig) {
      LaserFrame f;
      f.playerId = gun.playerId();
      f.weaponId = 0;
      f.team = (uint8_t)gun.team();
      f.shotSeq = (uint8_t)esp_random();
      laser.sendFrame(f);
#if IR_DUAL_FRAME
      delay(IR_DUAL_FRAME_GAP_MS);
      laser.sendFrame(f);
#endif
    }
  } else if (trig && gun.canFire()) {
    gun.consumeShot();
    LaserFrame f;
    f.playerId = gun.playerId();
    f.weaponId = 0;
    f.team = (uint8_t)gun.team();
    f.shotSeq = (uint8_t)esp_random();
    motorStart();
    laser.sendFrame(f);
#if IR_DUAL_FRAME
    delay(IR_DUAL_FRAME_GAP_MS);
    laser.sendFrame(f);
#endif
    // F 帧 1s 合并（开火统计粗粒度即可；TDMA 上行 1 帧/超帧，保护 H/D 关键帧容量）
    if (millis() - g_lastFire >= 1000) {
      tdmaSend(TF_FIRE, NULL, 0);
      g_lastFire = millis();
    }
    logBuf.log("F,%lu", millis() / 1000);
    audio.play(AudioPlayer::FX_SHOT);
  }

  // 4) 红外接收 → 本地扣血 → 上报 H（排队自身时隙）；阵亡 D 延迟双帧窗口
  LaserFrame hit;
  if (!laser.isTransmitting() && laser.poll(hit)) {
    if (g_pendDeath.valid && g_pendDeath.shooter == hit.playerId &&
        g_pendDeath.shotSeq == hit.shotSeq) {
      g_pendDeath.frames++;
    }
    bool killed = false;
    if (gun.applyHit(hit, killed)) {
      sendHit(hit.playerId, hit.weaponId, hit.shotSeq, hit.channel, gun.hp());
      logBuf.log("H,%lu,%u,%d", millis() / 1000, hit.playerId, hit.weaponId);
      if (killed) queueDeath(hit.playerId, hit.weaponId, hit.shotSeq,
                             hit.channel);
    }
  }

  // 5) TDMA 下行处理（广播窗收到的帧）
  TdmaFrame f;
  while (tdma.pollDownlink(f)) {
    handleTdmaFrame(f);
  }

  // 6) 声光反馈（按最近事件）
  handleFeedback();

  // 6.3) 音频 DMA 喂数 + 灯效时序
  audio.update();
  led.update();

  // 6.4) 阵亡上报（延迟双帧窗口 + 光强）
  deathUpdate();

  // 6.45) 无改装联动：阵亡/空弹 → 检测板断电；重生/装弹 → 恢复
#if GUN_ESPNOW_ENABLE
  gunPowerUpdate();
  gunEspNow.update();
#endif

  // 6.5) 电机联动定时（与红外发射并行）
  motorUpdate();

  // 6.6) 环境光采样（强光自动升远档）
  lightUpdate();

  // 6.7) 日志上传时机
  if (logBuf.usage() >= LOG_UPLOAD_FULL) {
    uploadLogs();
  } else if (logBuf.pending() &&
             millis() - g_lastLogUpload >= LOG_UPLOAD_IDLE_MS) {
    g_lastLogUpload = millis();
    uploadLogs();
  }

  // 6.8) 菜单按键扫描 + 超时返回 + 动作消费
  scanMenuButtons();
  menu.update();
  switch (menu.takeAction()) {
    case Menu::PAIR_HELMET: {
      tdmaSend(TF_PAIR, NULL, 0);
      logBuf.log("A,%lu", millis() / 1000);
      Serial.println("[menu] pair helmet requested");
      break;
    }
    case Menu::JOIN_MATCH: {
      g_registered = false;  // 触发重发 J → 服务器重发 welcome
      g_lastJoin = 0;
      Serial.println("[menu] join match: re-register");
      break;
    }
    default:
      break;
  }

  // 7) 时间状态（装弹计时）
  gun.update();

  // 8) 显示屏刷新（内部 200ms 节流 + 状态变化立即刷新）
  display.update(g_registered && tdma.assigned(), DEVICE_ID);

  delay(2);  // 节流主循环，兼顾 ISR 缓冲（TDMA 任务独立于本循环）
}
