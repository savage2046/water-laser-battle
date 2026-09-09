#pragma once
#include <Arduino.h>

// 统一的日志宏：串口（USB CDC 或 UART）可用时才输出。
// 板子用电池单独供电（无 USB 主机/未打开串口）时自动静默，不影响测试流程与 LED 指示。
#define DBG(...)                       \
  do {                                 \
    if (Serial) {                      \
      Serial.printf(__VA_ARGS__);      \
    }                                  \
  } while (0)
