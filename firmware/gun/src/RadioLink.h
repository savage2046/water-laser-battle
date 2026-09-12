#pragma once
#include <Arduino.h>
#include <RadioLib.h>

<<<<<<< HEAD
// 470MHz LoRa 链路：E22-400M22S（SX1268 芯片）SPI 直驱（RadioLib SX1262 类，
// 寄存器兼容；470MHz 在 SX1262 类频率范围内）。
// 一帧一个 LoRa 包（≤64 字节），LoRa 自带长度，无需分隔符。
class RadioLink {
 public:
  // 初始化 SX1262（参数来自 config.h：频率/带宽/SF/CR/同步字/功率）
=======
// 470MHz LoRa 链路：**U2 = Ra-01S（SX1268）** SPI 直驱（RadioLib）。
// 一帧一个 LoRa 包（≤64 字节），LoRa 自带长度，无需分隔符。
//
// ⚠️ 类必须与模块匹配：RadioLib 的 SX126x 家族用**版本字符串**做身份校验
//    （SX1262 类只认 "SX1261"、SX1268 类只认 "SX1268"、LLCC68 类只认 "LLCC68"），
//    拿错类 begin() 会重试 10 次后返回 -2 CHIP_NOT_FOUND。
//    Ra-01S 是 **SX1268** → 必须用 SX1268 类。早期版本用 SX1262 类，
//    表现为"芯片无应答"的假故障（焊接全好也永远 -2）。
class RadioLink {
 public:
  // 初始化 SX1268（参数来自 config.h：频率/带宽/SF/CR/同步字/功率）
>>>>>>> a6cdf1eb7eb9efd0fa4af8e183905e260cf2321d
  void begin();

  // 发送一帧（阻塞至发送完成，随后回到连续接收）
  void send(const char *frame);
  void sendFmt(const char *fmt, ...);

  // 非阻塞轮询接收：返回 true 表示收到一帧
  bool poll(char *out, size_t maxLen);

  bool isReady() { return _ready; }

<<<<<<< HEAD
  // TDMA MAC 直接访问 SX1262（独占使用；ALOHA 模式下勿与 send/poll 混用）
  SX1262 *getRadio() { return _radio; }

 private:
  SX1262 *_radio = nullptr;
=======
  // TDMA MAC 直接访问 SX126x（基类指针；独占使用，ALOHA 模式下勿与 send/poll 混用）
  SX126x *getRadio() { return _radio; }

 private:
  SX126x *_radio = nullptr;
>>>>>>> a6cdf1eb7eb9efd0fa4af8e183905e260cf2321d
  bool _ready = false;
  char _buf[96];
};

extern RadioLink radio;
