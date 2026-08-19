#include "TdmaProto.h"

uint8_t tdmaCrc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

void tdmaEncode(const TdmaFrame &f, uint8_t *buf) {
  buf[0] = f.devIdx;
  buf[1] = (uint8_t)((f.type << 3) | (f.flags & 0x07));
  buf[2] = (uint8_t)(f.seq >> 8);
  buf[3] = (uint8_t)(f.seq & 0xFF);
  memcpy(buf + 4, f.payload, 5);
  buf[9] = tdmaCrc8(buf, 9);
}

bool tdmaDecode(const uint8_t *buf, TdmaFrame &f) {
  if (tdmaCrc8(buf, 9) != buf[9]) return false;
  f.devIdx = buf[0];
  f.type = (uint8_t)(buf[1] >> 3);
  f.flags = (uint8_t)(buf[1] & 0x07);
  f.seq = (uint16_t)((buf[2] << 8) | buf[3]);
  memcpy(f.payload, buf + 4, 5);
  f.crc = buf[9];
  return true;
}

void tdmaMake(TdmaFrame &f, uint8_t devIdx, uint8_t type, uint8_t flags,
              uint16_t seq, const uint8_t *payload5) {
  f.devIdx = devIdx;
  f.type = type;
  f.flags = flags;
  f.seq = seq;
  if (payload5) {
    memcpy(f.payload, payload5, 5);
  } else {
    memset(f.payload, 0, 5);
  }
  f.crc = 0;  // 编码时计算
}
