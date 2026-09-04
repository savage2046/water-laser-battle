#include "LaserCodec.h"
#include "config.h"

LaserCodec laser;

// ---- 两路接收 ISR ----
// （friend 声明于 LaserCodec.h，勿加 static——否则与 friend 的 extern 声明冲突）
void IRAM_ATTR irEdgeISR940() {
  uint32_t now = micros();
  uint8_t lvl = digitalRead(PIN_IR_RX);
  uint8_t h = laser._head[0];
  uint8_t next = (h + 1) % LaserCodec::EDGE_BUF;
  if (next != laser._tail[0]) {
    laser._edgeTime[0][h] = now;
    laser._edgeLevel[0][h] = lvl;
    laser._head[0] = next;
  }
}

void IRAM_ATTR irEdgeISR850() {
  uint32_t now = micros();
  uint8_t lvl = digitalRead(PIN_IR_RX_850);
  uint8_t h = laser._head[1];
  uint8_t next = (h + 1) % LaserCodec::EDGE_BUF;
  if (next != laser._tail[1]) {
    laser._edgeTime[1][h] = now;
    laser._edgeLevel[1][h] = lvl;
    laser._head[1] = next;
  }
}

void LaserCodec::begin(uint8_t tx940, uint8_t tx850, uint8_t rx940,
                       uint8_t rx850, uint8_t powerPin, uint8_t pwr850A,
                       uint8_t pwr850B) {
  _tx940 = tx940;
  _tx850 = tx850;
  _rx940 = rx940;
  _rx850 = rx850;
  _powerPin = powerPin;
  _pwr850A = pwr850A;
  _pwr850B = pwr850B;
  pinMode(_tx940, OUTPUT);
  digitalWrite(_tx940, LOW);
  pinMode(_tx850, OUTPUT);
  digitalWrite(_tx850, LOW);
  pinMode(_rx940, INPUT_PULLUP);
  pinMode(_rx850, INPUT_PULLUP);
  if (_powerPin != 0xFF) {
    pinMode(_powerPin, OUTPUT);
    digitalWrite(_powerPin, LOW);
  }
  if (_pwr850A != 0xFF) {
    pinMode(_pwr850A, OUTPUT);
    digitalWrite(_pwr850A, LOW);
  }
  if (_pwr850B != 0xFF) {
    pinMode(_pwr850B, OUTPUT);
    digitalWrite(_pwr850B, LOW);
  }
  attachInterrupt(digitalPinToInterrupt(_rx940), irEdgeISR940, CHANGE);
  attachInterrupt(digitalPinToInterrupt(_rx850), irEdgeISR850, CHANGE);
  setPowerLevel(DEFAULT_POWER_LEVEL);
}

// 功率档位（全局 0..3，两通道独立映射）：
//   远距通道(38kHz/940nm): 档位≥2 → PIN_IR_POWER 拉高远档大电流（近档 0/1）
//   近距通道(56kHz/940nm，历史命名 _850): bit0=IR_PWR_850_A、bit1=IR_PWR_850_B，
//          00/01/10/11 → 0.5/1.0/1.5/2.0 × I_nom（I_nom=校准电流；默认档 1 保持 20m 边界）
//          仅 A 可用时退化为 2 档（档位≥1 → 1.0·I_nom）
void LaserCodec::setPowerLevel(uint8_t level) {
  _powerLevel = level;
  if (_powerPin != 0xFF) {
    digitalWrite(_powerPin, level >= 2 ? HIGH : LOW);
  }
  if (_pwr850A != 0xFF) {
    if (_pwr850B != 0xFF) {
      digitalWrite(_pwr850A, (level & 0x01) ? HIGH : LOW);
      digitalWrite(_pwr850B, (level & 0x02) ? HIGH : LOW);
    } else {
      digitalWrite(_pwr850A, level >= 1 ? HIGH : LOW);
    }
  }
}

// 双通道并行位发送状态（每通道独立时钟，发相同数据）
struct ChanTx {
  uint8_t pin;
  uint32_t halfUs;        // 载波半周期（config.h：IR_CARRIER_HALF_US=13 / _850=9）
  bool level = false;
  bool carrier = false;
  bool done = false;
  uint32_t flipAt = 0;
  uint32_t phaseUntil = 0;
  const uint8_t *bytes;
  uint8_t byteIdx = 0;
  int8_t bitPos = 7;
  bool curBit() { return (bytes[byteIdx] >> bitPos) & 1; }
  void advance() {
    bitPos--;
    if (bitPos < 0) {
      bitPos = 7;
      byteIdx++;
      if (byteIdx >= 5) done = true;
    }
  }
};

void LaserCodec::sendFrame(const LaserFrame &f) {
  _txBusy = true;

  // 引导码：两通道同时 9ms 载波 + 4.5ms 间隔（并行）
  uint32_t t0 = micros();
  uint32_t flip940 = t0, flip850 = t0;
  bool lvl940 = false, lvl850 = false;
  uint32_t carrierUntil = t0 + IR_PREAMBLE_US;
  while (micros() < carrierUntil) {
    uint32_t now = micros();
    if (now >= flip940) {
      lvl940 = !lvl940;
      digitalWrite(_tx940, lvl940);
      flip940 = now + IR_CARRIER_HALF_US;        // 38.46kHz（XL-IRM-V838M3 带通内）
    }
    if (now >= flip850) {
      lvl850 = !lvl850;
      digitalWrite(_tx850, lvl850);
      flip850 = now + IR_CARRIER_HALF_US_850;    // 55.6kHz（56k 接收头待定）
    }
  }
  digitalWrite(_tx940, LOW);
  digitalWrite(_tx850, LOW);
  delayMicroseconds(IR_PREAMBLE_GAP_US);

  // 完整 40bit 帧（两通道相同内容）：
  // B0=playerId高, B1=playerId低, B2=weapon/team/res, B3=shotSeq, chk
  uint8_t b0 = f.playerId >> 8;
  uint8_t b1 = f.playerId & 0xFF;
  uint8_t b2 = ((f.weaponId & 0x0F) << 4) | ((f.team & 0x03) << 2);
  uint8_t b3 = f.shotSeq;
  uint8_t chk = ~(uint8_t)(b0 ^ b1 ^ b2 ^ b3);
  uint8_t bytes[5] = { b0, b1, b2, b3, chk };

  // C++11 下含默认成员初始化器的结构体不是聚合体，不能用花括号初始化——
  // 改为默认构造后逐字段赋值（语义与原 10 字段列表一致）
  ChanTx chA, chB;
  chA.pin = _tx940;  chA.halfUs = IR_CARRIER_HALF_US; chA.bytes = bytes; chA.bitPos = 7;
  chB.pin = _tx850;  chB.halfUs = IR_CARRIER_HALF_US_850; chB.bytes = bytes; chB.bitPos = 7;
  uint32_t now0 = micros();
  chA.carrier = true;  chA.flipAt = now0;  chA.phaseUntil = now0 + IR_BIT_HIGH_US;
  chB.carrier = true;  chB.flipAt = now0;  chB.phaseUntil = now0 + IR_BIT_HIGH_US;

  while (!chA.done || !chB.done) {
    uint32_t now = micros();
    if (!chA.done) {
      if (chA.carrier) {
        if (now >= chA.flipAt) {
          chA.level = !chA.level;
          digitalWrite(chA.pin, chA.level);
          chA.flipAt = now + chA.halfUs;
        }
        if (now >= chA.phaseUntil) {
          chA.carrier = false;
          chA.level = false;
          digitalWrite(chA.pin, LOW);
          chA.phaseUntil = now + (chA.curBit() ? IR_BIT_ONE_GAP_US : IR_BIT_ZERO_GAP_US);
        }
      } else {
        if (now >= chA.phaseUntil) {
          chA.advance();
          if (chA.done) {
            digitalWrite(chA.pin, LOW);
          } else {
            chA.carrier = true;
            chA.flipAt = now;
            chA.phaseUntil = now + IR_BIT_HIGH_US;
          }
        }
      }
    }
    if (!chB.done) {
      if (chB.carrier) {
        if (now >= chB.flipAt) {
          chB.level = !chB.level;
          digitalWrite(chB.pin, chB.level);
          chB.flipAt = now + chB.halfUs;
        }
        if (now >= chB.phaseUntil) {
          chB.carrier = false;
          chB.level = false;
          digitalWrite(chB.pin, LOW);
          chB.phaseUntil = now + (chB.curBit() ? IR_BIT_ONE_GAP_US : IR_BIT_ZERO_GAP_US);
        }
      } else {
        if (now >= chB.phaseUntil) {
          chB.advance();
          if (chB.done) {
            digitalWrite(chB.pin, LOW);
          } else {
            chB.carrier = true;
            chB.flipAt = now;
            chB.phaseUntil = now + IR_BIT_HIGH_US;
          }
        }
      }
    }
  }
  _txBusy = false;
}

// 单通道解码完整 40bit 帧，返回 true 且 out 有效表示校验通过；outChannel 填通道号
bool LaserCodec::decodeChan40(LaserCodec::State &st, uint32_t &lastEdgeUs,
                         uint64_t &bitBuf, uint8_t &bitCnt,
                         const volatile uint32_t *edgeTime,
                         const volatile uint8_t *edgeLevel,
                         volatile uint8_t &head, volatile uint8_t &tail,
                         LaserFrame &out, uint8_t outChannel) {
  while (tail != head) {
    uint32_t t = edgeTime[tail];
    uint8_t lvl = edgeLevel[tail];
    tail = (tail + 1) % LaserCodec::EDGE_BUF;

    uint32_t dt = t - lastEdgeUs;
    lastEdgeUs = t;

    switch (st) {
      case LaserCodec::S_IDLE:
        if (lvl == LOW && dt > IR_PREAMBLE_US - IR_PULSE_TOL_US &&
            dt < IR_PREAMBLE_US + IR_PULSE_TOL_US) {
          st = LaserCodec::S_PREAMBLE;
        }
        break;
      case LaserCodec::S_PREAMBLE:
        if (lvl == HIGH && dt > IR_PREAMBLE_GAP_US - IR_PULSE_TOL_US &&
            dt < IR_PREAMBLE_GAP_US + IR_PULSE_TOL_US) {
          st = LaserCodec::S_DATA;
          bitBuf = 0;
          bitCnt = 0;
        } else {
          st = LaserCodec::S_IDLE;
        }
        break;
      case LaserCodec::S_DATA:
        if (lvl == LOW && dt > IR_BIT_HIGH_US - IR_PULSE_TOL_US &&
            dt < IR_BIT_HIGH_US + IR_PULSE_TOL_US) {
          st = LaserCodec::S_GAP;
        } else {
          st = LaserCodec::S_IDLE;
        }
        break;
      case LaserCodec::S_GAP:
        if (lvl == HIGH) {
          bitBuf = (bitBuf << 1) | (dt > IR_GAP_THRESH_US ? 1 : 0);
          bitCnt++;
          if (bitCnt >= 40) {
            st = LaserCodec::S_IDLE;
            bitCnt = 0;
            uint8_t b0 = (bitBuf >> 32) & 0xFF;
            uint8_t b1 = (bitBuf >> 24) & 0xFF;
            uint8_t b2 = (bitBuf >> 16) & 0xFF;
            uint8_t b3 = (bitBuf >> 8) & 0xFF;
            uint8_t chk = bitBuf & 0xFF;
            if (chk == (uint8_t)~(b0 ^ b1 ^ b2 ^ b3)) {
              out.playerId = (b0 << 8) | b1;
              out.weaponId = (b2 >> 4) & 0x0F;
              out.team = (b2 >> 2) & 0x03;
              out.shotSeq = b3;
              out.channel = outChannel;
              return true;
            }
          } else {
            st = LaserCodec::S_DATA;
          }
        } else {
          st = LaserCodec::S_IDLE;
        }
        break;
    }
  }
  return false;
}

bool LaserCodec::poll(LaserFrame &out) {
  // 两通道独立解码完整帧，任一成功即命中（距离互补）；channel 标记 0=940 1=850
  if (decodeChan40(_st[0], _lastEdgeUs[0], _bitBuf[0], _bitCnt[0],
                   _edgeTime[0], _edgeLevel[0], _head[0], _tail[0], out, 0)) {
    rxCount++;
    return true;
  }
  if (decodeChan40(_st[1], _lastEdgeUs[1], _bitBuf[1], _bitCnt[1],
                   _edgeTime[1], _edgeLevel[1], _head[1], _tail[1], out, 1)) {
    rxCount++;
    return true;
  }
  return false;
}
