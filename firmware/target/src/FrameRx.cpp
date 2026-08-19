#include "FrameRx.h"
#include "config.h"

FrameRx frameRx;

static void IRAM_ATTR irEdgeISR940() {
  FrameRx::Chan &ch = frameRx.channel(0);
  uint32_t now = micros();
  uint8_t lvl = digitalRead(ch.pin);
  uint8_t h = ch.head;
  uint8_t next = (h + 1) % 128;
  if (next != ch.tail) {
    ch.edgeTime[h] = now;
    ch.edgeLevel[h] = lvl;
    ch.head = next;
  }
}

static void IRAM_ATTR irEdgeISR850() {
  FrameRx::Chan &ch = frameRx.channel(1);
  uint32_t now = micros();
  uint8_t lvl = digitalRead(ch.pin);
  uint8_t h = ch.head;
  uint8_t next = (h + 1) % 128;
  if (next != ch.tail) {
    ch.edgeTime[h] = now;
    ch.edgeLevel[h] = lvl;
    ch.head = next;
  }
}

void FrameRx::begin(uint8_t rx940, uint8_t rx850) {
  _ch[0].pin = rx940;
  _ch[1].pin = rx850;
  pinMode(_ch[0].pin, INPUT);
  pinMode(_ch[1].pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(_ch[0].pin), irEdgeISR940, CHANGE);
  attachInterrupt(digitalPinToInterrupt(_ch[1].pin), irEdgeISR850, CHANGE);
}

// 单路解码完整 40bit 帧
bool FrameRx::Chan::decode(uint64_t &outBuf) {
  while (tail != head) {
    uint32_t t = edgeTime[tail];
    uint8_t lvl = edgeLevel[tail];
    tail = (tail + 1) % 128;
    uint32_t dt = t - lastEdgeUs;
    lastEdgeUs = t;
    switch (st) {
      case S_IDLE:
        if (lvl == LOW && dt > IR_PREAMBLE_US - IR_PULSE_TOL_US &&
            dt < IR_PREAMBLE_US + IR_PULSE_TOL_US) st = S_PREAMBLE;
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

bool FrameRx::poll(LaserFrame &out) {
  for (uint8_t i = 0; i < 2; i++) {
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
        out.channel = i;  // 0=940nm 1=850nm
        return true;
      }
    }
  }
  return false;
}
