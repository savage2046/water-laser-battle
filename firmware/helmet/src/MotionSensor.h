#pragma once
#include <Arduino.h>

// 运动传感器（LIS3DH 加速度计，软件 I2C + INT1 运动中断）。
// 用途：静止时 GPS 断电，运动中断唤醒 GPS 按需定位。
class MotionSensor {
 public:
  // sda/scl：软件 I2C 引脚（与 OLED 分线，避免 U8g2 SW_I2C 与 Wire 冲突）
  // intPin：LIS3DH INT1 输出（运动中断，接 ESP32 中断引脚）
  bool begin(uint8_t sda, uint8_t scl, uint8_t intPin);

  // 是否有运动中断待处理（loop 轮询），返回后内部标志清除
  bool takeMotion();

  // 当前加速度（可选调试，mg 单位）
  int16_t accelX, accelY, accelZ;

 private:
  void writeReg(uint8_t reg, uint8_t val);
  uint8_t readReg(uint8_t reg);

  uint8_t _sda, _scl, _intPin;
  volatile bool _motion = false;
  bool _ok = false;

  friend void motionISR();
};

extern MotionSensor motion;
