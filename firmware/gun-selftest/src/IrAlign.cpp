#include "IrAlign.h"

// ============================================================================
// 940nm / 38kHz 光路矫正发射实现
// 编码与 firmware/gun/src/LaserCodec.cpp 逐位一致（MSB first，5 字节 + 校验）
// ============================================================================

IrAlign g_ir;

void IrAlign::begin() {
  pinMode(PIN_IR_TX_FAR, OUTPUT);
  digitalWrite(PIN_IR_TX_FAR, LOW);
  pinMode(PIN_IR_TX_NEAR, OUTPUT);
  digitalWrite(PIN_IR_TX_NEAR, LOW);

  // 近距通道功率档：00 = 最低（0.5×I_nom）；远距通道档位 0/1 均为近档（低电流）
  pinMode(PIN_IR_PWR_NEAR_A, OUTPUT);
  pinMode(PIN_IR_PWR_NEAR_B, OUTPUT);
#if IR_ALIGN_POWER_LEVEL & 0x01
  digitalWrite(PIN_IR_PWR_NEAR_A, HIGH);
#else
  digitalWrite(PIN_IR_PWR_NEAR_A, LOW);
#endif
#if IR_ALIGN_POWER_LEVEL & 0x02
  digitalWrite(PIN_IR_PWR_NEAR_B, HIGH);
#else
  digitalWrite(PIN_IR_PWR_NEAR_B, LOW);
#endif
#if PIN_IR_POWER_FAR != 0xFF
  pinMode(PIN_IR_POWER_FAR, OUTPUT);
  digitalWrite(PIN_IR_POWER_FAR, IR_ALIGN_POWER_LEVEL >= 2 ? HIGH : LOW);
#endif
}

void IrAlign::start() {
  if (_task) return;
  // core0 优先级 10：与 LoRa 任务(core1/prio5) 物理隔离；整段 700ms 忙等，
  // 只在周期内"灭"的 0.3s 让出 CPU（IDLE0 看门狗 5s，安全）
  if (xTaskCreatePinnedToCore(taskEntry, "iralign", 3072, this, 10, &_task, 0) !=
      pdPASS) {
    _task = nullptr;
  }
}

void IrAlign::taskEntry(void *arg) { ((IrAlign *)arg)->run(); }

// 每 IR_ALIGN_PERIOD_MS 一个周期：前 IR_ALIGN_ON_MS 持续连发编码帧（光信号常亮），
// 剩余时间灭灯。
//   连发可行性：解码器只在 S_IDLE 里等"引导码结束"（LOW 电平、间隔 ≈9ms），
//   上一帧结尾的空闲电平不会误触发，因此背靠背帧可以逐帧被反复解码。
void IrAlign::run() {
  for (;;) {
    if (!_enabled) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    const uint32_t t0 = millis();
    const uint32_t onTarget = t0 + IR_ALIGN_ON_MS;
    uint32_t frames = 0;
    // 亮 0.7s：背靠背发帧（收尾不截断帧，实际时长 700~800ms，实测值打进报告）
    while ((int32_t)(onTarget - millis()) > 0) {
      sendBurst();
      frames++;
      if (frames > 32) break;          // 兜底，防异常时长
    }
    pinsWrite(false);
    _framesPerOn = frames;
    _lastOnMs = (uint32_t)(millis() - t0);

    const uint32_t spent = (uint32_t)(millis() - t0);
    const uint32_t wait = (spent < IR_ALIGN_PERIOD_MS)
                              ? (IR_ALIGN_PERIOD_MS - spent)
                              : 1;
    vTaskDelay(pdMS_TO_TICKS(wait));   // 灭 0.3s（同时让出 IDLE0）
  }
}

void IrAlign::setChannel(uint8_t ch) {
  if (ch > IR_ALIGN_CH_BOTH) ch = IR_ALIGN_CH_BOTH;
  _ch = ch;
}

void IrAlign::pinsWrite(bool level) {
  const uint8_t ch = _ch;
  if (ch == 0 || ch == IR_ALIGN_CH_BOTH) digitalWrite(PIN_IR_TX_NEAR, level);
  if (ch == 1 || ch == IR_ALIGN_CH_BOTH) digitalWrite(PIN_IR_TX_FAR, level);
}

// 在 [startUs, startUs+durUs) 内输出半周期 IR_ALIGN_HALF_US 的方波（忙等）
void IrAlign::carrierFor(uint32_t startUs, uint32_t durUs) {
  const uint32_t until = startUs + durUs;
  uint32_t flip = startUs;
  bool lvl = false;
  while ((int32_t)(until - micros()) > 0) {
    const uint32_t now = micros();
    if ((int32_t)(now - flip) >= 0) {
      lvl = !lvl;
      pinsWrite(lvl);
      flip = now + IR_ALIGN_HALF_US;
    }
  }
}

void IrAlign::frameBytes(uint8_t out[5]) const {
#if IR_ALIGN_USE_DEV_IDX
  const uint16_t pid = DEV_IDX;
#else
  const uint16_t pid = 0;
#endif
  const uint8_t b0 = (uint8_t)(pid >> 8);
  const uint8_t b1 = (uint8_t)(pid & 0xFF);
  const uint8_t b2 = (uint8_t)(((IR_ALIGN_WEAPON & 0x0F) << 4) |
                               ((IR_ALIGN_TEAM & 0x03) << 2));
  const uint8_t b3 = (uint8_t)_shotSeq;
  out[0] = b0;
  out[1] = b1;
  out[2] = b2;
  out[3] = b3;
  out[4] = (uint8_t)~(uint8_t)(b0 ^ b1 ^ b2 ^ b3);   // 与 LaserCodec 同口径
}

void IrAlign::sendBurst() {
  if (_busy) return;        // 另一处在发（周期任务 vs 命令 b）：跳过，避免抢同一组引脚
  _busy = true;

  uint8_t bytes[5];
  frameBytes(bytes);

  const uint32_t t0 = micros();

  // 1) 引导码：9ms 连续载波
  carrierFor(t0, IR_PREAMBLE_US);
  pinsWrite(false);
  // 2) 引导码后间隔 4.5ms（低电平）
  delayMicroseconds(IR_PREAMBLE_GAP_US);

  // 3) 40bit（MSB first，5 字节）
  for (uint8_t i = 0; i < 40; i++) {
    const uint8_t byte = bytes[i >> 3];
    const uint8_t bit = (uint8_t)((byte >> (7 - (i & 7))) & 1);
    const uint32_t s = micros();
    carrierFor(s, IR_BIT_HIGH_US);            // 每位起始 560µs 载波
    pinsWrite(false);
    delayMicroseconds(bit ? IR_BIT_ONE_GAP_US : IR_BIT_ZERO_GAP_US);
  }
  pinsWrite(false);

  _lastBurstUs = (uint32_t)(micros() - t0);
  _lastBurstMs = millis();
  _shotSeq++;
  _txCount++;
  _busy = false;
}
