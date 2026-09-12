#pragma once
#include <Arduino.h>
#include <RadioLib.h>

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
  void begin();

  // 开机自检：读 0x0320 版本串 + 0x0740/0x0741 同步字 + GetDeviceErrors，打印一行判据。
  // 版本串必须是 "SX1268"（这一条同时证明 SPI 四线 + 供电 + 复位都正常）。
  // 可重复调用（排查时随时手动跑一次），返回是否通过。
  bool selfCheck();

  // 发送一帧（阻塞至发送完成，随后回到连续接收）
  void send(const char *frame);
  void sendFmt(const char *fmt, ...);

  // 非阻塞轮询接收：返回 true 表示收到一帧
  bool poll(char *out, size_t maxLen);

  bool isReady() { return _ready; }

  // TDMA MAC 直接访问 SX126x（基类指针；独占使用，ALOHA 模式下勿与 send/poll 混用）
  SX126x *getRadio() { return _radio; }

 private:
  SX126x *_radio = nullptr;
  Module *_mod = nullptr;   // 自己留一份（SX126x 的 mod 是 private/getMod() 是 protected，
                            // 但 Module 的 SPIreadRegisterBurst/SPIreadStream 是公开的，
                            // 用于开机自检直接读寄存器）
  bool _ready = false;
  char _buf[96];
};

extern RadioLink radio;