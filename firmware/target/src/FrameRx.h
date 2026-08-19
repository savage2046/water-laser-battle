#pragma once
#include <Arduino.h>

// 红外激光帧结构（与枪端/头盔一致：双波段传相同完整 40bit 帧）
struct LaserFrame {
  uint16_t playerId;
  uint8_t weaponId;
  uint8_t team;
  uint8_t shotSeq;
  uint8_t channel;  // 0=940nm 1=850nm
};

// 双波段解码（2 通道：0=940nm 中心颗，1=850nm）：
// 两通道各解码完整 40bit 帧，任一成功且校验通过 → 命中（距离互补）。
class FrameRx {
 public:
  struct Chan {
    uint8_t pin;
    volatile uint32_t edgeTime[128];
    volatile uint8_t edgeLevel[128];
    volatile uint8_t head = 0, tail = 0;
    enum State { S_IDLE, S_PREAMBLE, S_DATA, S_GAP };
    State st = S_IDLE;
    uint32_t lastEdgeUs = 0;
    uint64_t bitBuf = 0;
    uint8_t bitCnt = 0;
    bool decode(uint64_t &outBuf);
  };

  void begin(uint8_t rx940, uint8_t rx850);

  // 非阻塞轮询：任一通道成功返回 true
  bool poll(LaserFrame &out);

  Chan &channel(uint8_t i) { return _ch[i]; }

 private:
  Chan _ch[2];
};

extern FrameRx frameRx;
