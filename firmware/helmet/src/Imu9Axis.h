#pragma once
#include <Arduino.h>

// 9 轴 IMU（MPU9250：加速+陀螺+磁力 AK8963），记录头盔朝向。
// 与 LIS3DH 共享软件 I2C 总线（地址不同）；VCC 由 GPIO 按需上电（省电）。
// 姿态：pitch/roll 由加速度计重力分量求，yaw 由磁力计投影水平面求（磁北为 0）。
class Imu9Axis {
 public:
  // sda/scl：共享软件 I2C 引脚（LIS3DH 同线）；pwrPin：VCC 电源开关（0xFF=常开）
  void begin(uint8_t sda, uint8_t scl, uint8_t pwrPin);

  // 上电 + 初始化（读取前调用）
  bool powerOn();

  // 断电（省电：平时不工作）
  void powerOff();

  // 读取 9 轴并融合出朝向（需先 powerOn）
  // 输出整数度：yaw 0-359（磁北为 0，顺时针），pitch -90..90，roll -180..180
  void readOrientation(int16_t &yaw, int16_t &pitch, int16_t &roll);

  bool ok() { return _ok; }

 private:
  void writeReg(uint8_t addr, uint8_t reg, uint8_t val);
  uint8_t readReg(uint8_t addr, uint8_t reg);
  void readBuf(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n);

  uint8_t _sda, _scl, _pwrPin;
  bool _ok = false;
  bool _powered = false;

  int16_t _ax, _ay, _az;
  int16_t _mx, _my, _mz;
};

extern Imu9Axis imu;
