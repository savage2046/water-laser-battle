#pragma once
#include <Arduino.h>
#include <RadioLib.h>

// 470MHz LoRa 链路（网关多射频版）：**Ra-01S / SX1268** SPI 直驱（RadioLib）。
// 多实例：每射频一个 RadioLink，SPI 总线共享（SCLK/MOSI/MISO 相同，NSS 各自选通）。
// 频率由开机自检自动分配（begin(freqMhz)），引脚由槽位表构造传入。
//
// ⚠️ 类必须与模块匹配：RadioLib 的 SX126x 家族用**版本字符串**做身份校验
//    （SX1262 类只认 "SX1261"、SX1268 类只认 "SX1268"、LLCC68 类只认 "LLCC68"），
//    拿错类 begin() 会重试 10 次后返回 -2 CHIP_NOT_FOUND。Ra-01S 是 SX1268。
class RadioLink {
 public:
  RadioLink() = default;
  RadioLink(uint8_t nss, int8_t rst, uint8_t busy, int8_t dio1 = -1)
      : _nss(nss), _rst(rst), _busy(busy), _dio1(dio1) {}

  // 初始化 SX1268（频率参数化；其余参数来自 config.h）。
  // 调用前需已 SPI.begin（main 自检阶段完成）；返回是否成功。
  bool begin(float freqMhz);

  // 开机自检：读 0x0320 版本串 + 0x0740/0x0741 同步字 + GetDeviceErrors，打印一行判据。
  // 版本串必须是 "SX1268"（同时证明该射频的 SPI/供电/复位正常）。
  // 多射频板建议每个射频都跑一次；可重复调用。返回是否通过。
  bool selfCheck();

  bool isReady() { return _ready; }

  // TDMA MAC 直接访问 SX126x（基类指针；独占使用）
  SX126x *getRadio() { return _radio; }

 private:
  uint8_t _nss = 0xFF;
  uint8_t _busy = 0xFF;
  int8_t _rst = -1;
  int8_t _dio1 = -1;
  SX126x *_radio = nullptr;
  Module *_mod = nullptr;   // 自己留一份（SX126x 的 mod 是 private/getMod() 是 protected，
                            // 但 Module 的 SPIreadRegisterBurst/SPIreadStream 是公开的，
                            // 供 selfCheck 直接读寄存器）
  bool _ready = false;
};