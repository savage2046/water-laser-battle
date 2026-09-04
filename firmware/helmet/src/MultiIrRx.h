#pragma once
#include <Arduino.h>

// 红外激光帧结构（与枪端 LaserCodec 一致：双载波传相同完整 40bit 帧，波长统一 940nm）
struct LaserFrame {
  uint16_t playerId;
  uint8_t weaponId;
  uint8_t team;
  uint8_t shotSeq;
  uint8_t channel;  // 0=38kHz 远距 1=56kHz 近距（均 940nm；远距击杀优先）
};

// 多路红外接收解码（双载波冗余）：
//   38kHz 通道(38kHz IRM：XL-IRM-V838M3/TR 成兴光，兜底 TSOP38238) 与 56kHz 通道
//   (56kHz IRM：SMD 首选 TSOP39256/75556) 各传相同完整帧（波长均 940nm）。
//   任一通道解码成功 + 校验通过 → 命中（近距靠 56kHz 宽光束，远距靠 38kHz 透镜通道）。
// 通道布局：0/1 = 38kHz 前/后，2/3 = 56kHz 前/后（最多 4 路）。
class MultiIrRx {
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
    // 解码完整 40bit 帧：返回 true 且 out 有效（校验在 poll 做）
    bool decode(uint64_t &outBuf);
  };

  static const uint8_t MAX_CH = 4;

  void begin(const uint8_t *rxPins, uint8_t count);
  // 非阻塞轮询所有通道：任一通道解码成功返回 true
  bool poll(LaserFrame &out);

  // 供 ISR 访问
  Chan &channel(uint8_t i) { return _ch[i]; }

 private:
  Chan _ch[MAX_CH];
  uint8_t _count = 0;
};

extern MultiIrRx irRx;
