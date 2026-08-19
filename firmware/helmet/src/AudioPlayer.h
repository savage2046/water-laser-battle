#pragma once
#include <Arduino.h>

// 音效播放：MAX98357A（I2S 功放）+ 8Ω 小喇叭。
// 音效为启动时合成的 8kHz 16bit 单声道 PCM（放 RAM，短音效内存占用小）。
// 播放为非阻塞（update() 轮询喂 DMA），不卡主循环。
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

  // bclk/ws/dout：I2S 引脚（MAX98357A BCLK/LRC/DIN）
  void begin(uint8_t bclk, uint8_t ws, uint8_t dout);

  // 触发音效（非阻塞）
  void play(Fx fx);

  // 主循环调用：向 DMA 喂数据
  void update();

  bool playing() { return _pos < _len; }

 private:
  void synthAll();

  static const uint16_t MAX_SAMPLES = 4000;  // 0.5s @8kHz
  int16_t _buf[MAX_SAMPLES];
  uint32_t _pos = 0;
  uint32_t _len = 0;
  bool _ok = false;
};

extern AudioPlayer audio;
