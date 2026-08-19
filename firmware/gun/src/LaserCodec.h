#pragma once
#include <Arduino.h>

// 红外激光帧结构（双波段冗余：两通道传相同完整 40bit 帧）
// playerId(16) + weaponId(4) + team(2) + res(2) + shotSeq(8) + chk(8)
struct LaserFrame {
  uint16_t playerId;   // 射手 ID
  uint8_t weaponId;    // 武器类型 0步枪 1手枪 2狙击 3霰弹 4机枪
  uint8_t team;        // 队伍 0/1，3=无
  uint8_t shotSeq;     // 射击序号（每次扣扳机随机，接收方精确去重）
  uint8_t channel;     // 接收通道：0=940nm 1=850nm（发射时忽略；940 击杀优先）
};

// 双波段红外编解码：38kHz(940nm, 加透镜远距) 与 56kHz(850nm, 宽光束近距)
// 两通道**并行发射相同帧**（距离互补）；接收端任一通道解码成功即命中。
class LaserCodec {
 public:
  // tx940/tx850：两个波段发射引脚；rx940/rx850：两个波段接收引脚（中断）
  void begin(uint8_t tx940, uint8_t tx850, uint8_t rx940, uint8_t rx850,
             uint8_t powerPin = 0xFF);

  // 发射一帧（两通道并行 ~80ms），发射期间忽略接收
  void sendFrame(const LaserFrame &f);

  // 作用范围功率档位：0近 1标准 2远（切换 PIN_IR_POWER 驱动电流）
  void setPowerLevel(uint8_t level);

  // 非阻塞轮询：任一通道解码成功返回 true
  bool poll(LaserFrame &out);

  // 发射是否进行中
  bool isTransmitting() { return _txBusy; }

  // 统计：接收帧数（调试用）
  uint32_t rxCount = 0;

 private:
  uint8_t _tx940, _tx850, _rx940, _rx850, _powerPin;
  uint8_t _powerLevel = 1;
  volatile bool _txBusy = false;

  // 两个接收通道的边沿缓冲（ISR 写入，poll 消费）
  static const uint8_t EDGE_BUF = 128;  // 40bit×2边沿+引导码=82，足够
  volatile uint32_t _edgeTime[2][EDGE_BUF];
  volatile uint8_t _edgeLevel[2][EDGE_BUF];
  volatile uint8_t _head[2] = {0, 0}, _tail[2] = {0, 0};

  // 两通道解码状态机
  enum State { S_IDLE, S_PREAMBLE, S_DATA, S_GAP };
  State _st[2] = {S_IDLE, S_IDLE};
  uint32_t _lastEdgeUs[2] = {0, 0};
  uint64_t _bitBuf[2] = {0, 0};
  uint8_t _bitCnt[2] = {0, 0};

  friend void irEdgeISR940();
  friend void irEdgeISR850();
};

// 全局实例
extern LaserCodec laser;
