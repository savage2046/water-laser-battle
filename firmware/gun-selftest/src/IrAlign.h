#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
// 940nm / 38kHz 光路矫正发射（T5）
//
// 每 1000ms 一个周期：**光信号持续 700ms**（周期内亮 0.7s / 灭 0.3s）。
// 点亮期间"背靠背"连发与正式固件 **逐位相同** 的 NEC 风格编码帧
// （引导码 9ms + 4.5ms 间隔 + 40bit：playerId|weapon|team|shotSeq|chk），
// 载波 38.46kHz，**功率档置最低（0）**，用于：
//   - 目视对准：0.7s 常亮比单帧 80ms 好找得多（手机摄像头可见 940nm 紫光）；
//   - 链路验证：接收端（头盔/校准靶）能反复解出完整帧并给出反馈；
//   - 检查发射驱动链路（GPIO → 限流电阻 → MOSFET → LED）与光路/亮度。
//
// 任务放置：**core0 优先级 10**（与 LoRa 设备任务 core1 完全分离）。
//   38.46kHz 载波半周期只有 13µs，任何抢占都会让载波瞬时跑偏、接收头带通
//   （37.9kHz±2~3kHz）解不出来；放到另一个核上做整段忙等，既不干扰 470M
//   时隙收发，也只在"灭"的 0.3s 里让出 IDLE0（远小于 5s 任务看门狗）。
//   代价：与 2.4GHz WiFi/ESP-NOW 栈共用 core0（T3 只在开机跑一次），
//   偶发抢占可能让个别编码帧解不出 —— 报告里能看到实际发出帧数。
// ============================================================================

class IrAlign {
 public:
  // 引脚初始化 + 功率档置最低 + 建立任务
  void begin();
  void start();

  // 通道：0=仅近距(G47) 1=仅远距(G13) 2=两个都发
  void setChannel(uint8_t ch);
  uint8_t channel() const { return _ch; }

  void setEnabled(bool on) { _enabled = on; }
  bool enabled() const { return _enabled; }

  uint32_t txCount() const { return _txCount; }
  uint32_t lastBurstUs() const { return _lastBurstUs; }
  uint32_t lastBurstMs() const { return _lastBurstMs; }
  uint32_t lastOnMs() const { return _lastOnMs; }       // 上一周期实测"亮"的时长
  uint32_t framesPerOn() const { return _framesPerOn; } // 上一周期发出的帧数
  uint16_t lastShotSeq() const { return _shotSeq; }

  // 立即发一帧（阻塞；供任务或串口命令手动触发）
  void sendBurst();

  // 帧内 5 字节（打印/核对用）
  void frameBytes(uint8_t out[5]) const;

 private:
  static void taskEntry(void *arg);
  void run();
  void pinsWrite(bool level);
  void carrierFor(uint32_t startUs, uint32_t durUs);

  volatile uint8_t _ch = IR_ALIGN_CH;
  volatile bool    _enabled = true;
  volatile uint32_t _txCount = 0;
  volatile uint32_t _lastBurstUs = 0;
  volatile uint32_t _lastBurstMs = 0;
  volatile uint32_t _lastOnMs = 0;
  volatile uint32_t _framesPerOn = 0;
  volatile uint16_t _shotSeq = 0;
  volatile bool    _busy = false;   // 防重入：周期任务与命令 b 同时驱动同一组引脚
  TaskHandle_t _task = nullptr;
};

extern IrAlign g_ir;
