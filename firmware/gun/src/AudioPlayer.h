#pragma once
#include <Arduino.h>

// 音效播放：MAX98357A（I2S 功放）+ 8Ω 小喇叭（与头盔共用设计）。
// 音效为 play() 时即时合成的 8kHz 16bit 单声道 PCM；非阻塞（update 喂 DMA）。
class AudioPlayer {
 public:
  enum Fx {
    FX_SHOT,     // 射击：噪声衰减
    FX_HIT,      // 命中：短哔
    FX_DEATH,    // 阵亡：下滑音
    FX_RESPAWN,  // 重生：上滑音
    FX_START,    // 对局开始：双音
    FX_END,      // 对局结束：长单音
    FX_CLICK,    // 菜单/确认：短音
    FX_COUNT
  };

  void begin(uint8_t bclk, uint8_t ws, uint8_t dout);
  void play(Fx fx);
  void update();
  bool playing() { return _pos < _len; }

 private:
  int16_t _buf[4000];
  uint32_t _pos = 0;
  uint32_t _len = 0;
  bool _ok = false;
};

extern AudioPlayer audio;
