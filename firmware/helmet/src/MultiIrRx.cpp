#include "MultiIrRx.h"
#include "config.h"

MultiIrRx irRx;

// 每路 ISR：记录边沿（时间, 电平）进环形缓冲。最多 4 路，用统一模板。
static void IRAM_ATTR irEdgeISR0() { MultiIrRx::Chan &ch = irRx.channel(0); uint32_t now = micros(); uint8_t lvl = digitalRead(ch.pin); uint8_t h = ch.head; uint8_t next = (h + 1) % 128; if (next != ch.tail) { ch.edgeTime[h] = now; ch.edgeLevel[h] = lvl; ch.head = next; } }
static void IRAM_ATTR irEdgeISR1() { MultiIrRx::Chan &ch = irRx.channel(1); uint32_t now = micros(); uint8_t lvl = digitalRead(ch.pin); uint8_t h = ch.head; uint8_t next = (h + 1) % 128; if (next != ch.tail) { ch.edgeTime[h] = now; ch.edgeLevel[h] = lvl; ch.head = next; } }
static void IRAM_ATTR irEdgeISR2() { MultiIrRx::Chan &ch = irRx.channel(2); uint32_t now = micros(); uint8_t lvl = digitalRead(ch.pin); uint8_t h = ch.head; uint8_t next = (h + 1) % 128; if (next != ch.tail) { ch.edgeTime[h] = now; ch.edgeLevel[h] = lvl; ch.head = next; } }
static void IRAM_ATTR irEdgeISR3() { MultiIrRx::Chan &ch = irRx.channel(3); uint32_t now = micros(); uint8_t lvl = digitalRead(ch.pin); uint8_t h = ch.head; uint8_t next = (h + 1) % 128; if (next != ch.tail) { ch.edgeTime[h] = now; ch.edgeLevel[h] = lvl; ch.head = next; } }

void MultiIrRx::begin(const uint8_t *rxPins, uint8_t count) {
  _count = min(count, (uint8_t)MAX_CH);
  for (uint8_t i = 0; i < _count; i++) {
    _ch[i].pin = rxPins[i];
    pinMode(_ch[i].pin, INPUT_PULLUP);
    _ch[i].st = Chan::S_IDLE;
    _ch[i].head = _ch[i].tail = 0;
  }
  if (_count > 0) attachInterrupt(digitalPinToInterrupt(_ch[0].pin), irEdgeISR0, CHANGE);
  if (_count > 1) attachInterrupt(digitalPinToInterrupt(_ch[1].pin), irEdgeISR1, CHANGE);
  if (_count > 2) attachInterrupt(digitalPinToInterrupt(_ch[2].pin), irEdgeISR2, CHANGE);
  if (_count > 3) attachInterrupt(digitalPinToInterrupt(_ch[3].pin), irEdgeISR3, CHANGE);
}

// 单路解码完整 40bit 帧：帧 = 9ms 低 + 4.5ms 高 + 40bit
// 返回 true 表示 40bit 收齐（原始位存 outBuf，校验由调用方做）
bool MultiIrRx::Chan::decode(uint64_t &outBuf) {
  while (tail != head) {
    uint32_t t = edgeTime[tail];
    uint8_t lvl = edgeLevel[tail];
    tail = (tail + 1) % 128;

    uint32_t dt = t - lastEdgeUs;
    lastEdgeUs = t;

    switch (st) {
      case S_IDLE:
        if (lvl == LOW && dt > IR_PREAMBLE_US - IR_PULSE_TOL_US &&
            dt < IR_PREAMBLE_US + IR_PULSE_TOL_US) {
          st = S_PREAMBLE;
        }
        break;
      case S_PREAMBLE:
        if (lvl == HIGH && dt > IR_PREAMBLE_GAP_US - IR_PULSE_TOL_US &&
            dt < IR_PREAMBLE_GAP_US + IR_PULSE_TOL_US) {
          st = S_DATA;
          bitBuf = 0;
          bitCnt = 0;
        } else {
          st = S_IDLE;
        }
        break;
      case S_DATA:
        if (lvl == LOW && dt > IR_BIT_HIGH_US - IR_PULSE_TOL_US &&
            dt < IR_BIT_HIGH_US + IR_PULSE_TOL_US) {
          st = S_GAP;
        } else {
          st = S_IDLE;
        }
        break;
      case S_GAP:
        if (lvl == HIGH) {
          bitBuf = (bitBuf << 1) | (dt > IR_GAP_THRESH_US ? 1 : 0);
          bitCnt++;
          if (bitCnt >= 40) {
            st = S_IDLE;
            bitCnt = 0;
            outBuf = bitBuf;
            return true;
          }
          st = S_DATA;
        } else {
          st = S_IDLE;
        }
        break;
    }
  }
  return false;
}

bool MultiIrRx::poll(LaserFrame &out) {
  // 所有通道独立解码，任一成功且校验通过即命中；通道 0/1=38kHz（远距），2/3=56kHz（近距）
  for (uint8_t i = 0; i < _count; i++) {
    uint64_t buf = 0;
    if (_ch[i].decode(buf)) {
      uint8_t b0 = (buf >> 32) & 0xFF;
      uint8_t b1 = (buf >> 24) & 0xFF;
      uint8_t b2 = (buf >> 16) & 0xFF;
      uint8_t b3 = (buf >> 8) & 0xFF;
      uint8_t chk = buf & 0xFF;
      if (chk == (uint8_t)~(b0 ^ b1 ^ b2 ^ b3)) {
        out.playerId = (b0 << 8) | b1;
        out.weaponId = (b2 >> 4) & 0x0F;
        out.team = (b2 >> 2) & 0x03;
        out.shotSeq = b3;
        out.channel = (i < 2) ? 0 : 1;  // 0/1=38kHz 远距, 2/3=56kHz 近距
        return true;
      }
    }
  }
  return false;
}
