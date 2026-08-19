#pragma once
#include <Arduino.h>
#include <RadioLib.h>

// 470MHz LoRa 链路（网关多射频版）：SX1262 SPI 直驱（RadioLib）。
// 多实例：每射频一个 RadioLink，SPI 总线共享（SCLK/MOSI/MISO 相同，NSS 各自选通）。
// 频率由开机自检自动分配（begin(freqMhz)），引脚由槽位表构造传入。
class RadioLink {
 public:
  RadioLink() = default;
  RadioLink(uint8_t nss, int8_t rst, uint8_t busy, int8_t dio1 = -1)
      : _nss(nss), _rst(rst), _busy(busy), _dio1(dio1) {}

  // 初始化 SX1262（频率参数化；其余参数来自 config.h）。
  // 调用前需已 SPI.begin（main 自检阶段完成）；返回是否成功。
  bool begin(float freqMhz);

  bool isReady() { return _ready; }

  // TDMA MAC 直接访问 SX1262（独占使用）
  SX1262 *getRadio() { return _radio; }

 private:
  uint8_t _nss = 0xFF;
  uint8_t _busy = 0xFF;
  int8_t _rst = -1;
  int8_t _dio1 = -1;
  SX1262 *_radio = nullptr;
  bool _ready = false;
};
