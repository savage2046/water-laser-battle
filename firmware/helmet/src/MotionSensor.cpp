#include "MotionSensor.h"
#include <Wire.h>

MotionSensor motion;

static void IRAM_ATTR motionISR() { motion._motion = true; }

// 软件 I2C 位操作（LIS3DH 只需低频访问：初始化 + 清中断）
static void i2cStart(uint8_t sda, uint8_t scl) {
  digitalWrite(sda, HIGH);
  digitalWrite(scl, HIGH);
  delayMicroseconds(5);
  digitalWrite(sda, LOW);
  delayMicroseconds(5);
  digitalWrite(scl, LOW);
}
static void i2cStop(uint8_t sda, uint8_t scl) {
  digitalWrite(sda, LOW);
  digitalWrite(scl, HIGH);
  delayMicroseconds(5);
  digitalWrite(sda, HIGH);
  delayMicroseconds(5);
}
static bool i2cWriteByte(uint8_t sda, uint8_t scl, uint8_t v) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(sda, (v >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(scl, HIGH);
    delayMicroseconds(4);
    digitalWrite(scl, LOW);
    delayMicroseconds(2);
  }
  // ACK（释放 SDA 读应答）
  pinMode(sda, INPUT);
  delayMicroseconds(2);
  digitalWrite(scl, HIGH);
  delayMicroseconds(4);
  bool ack = digitalRead(sda) == LOW;
  digitalWrite(scl, LOW);
  pinMode(sda, OUTPUT);
  return ack;
}
static uint8_t i2cReadByte(uint8_t sda, uint8_t scl, bool last) {
  uint8_t v = 0;
  pinMode(sda, INPUT);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(scl, HIGH);
    delayMicroseconds(4);
    v = (v << 1) | (digitalRead(sda) ? 1 : 0);
    digitalWrite(scl, LOW);
    delayMicroseconds(2);
  }
  // ACK/NACK
  pinMode(sda, OUTPUT);
  digitalWrite(sda, last ? HIGH : LOW);
  delayMicroseconds(2);
  digitalWrite(scl, HIGH);
  delayMicroseconds(4);
  digitalWrite(scl, LOW);
  pinMode(sda, INPUT);
  return v;
}

#define LIS3DH_ADDR 0x18        // SA0 接地
#define REG_WHO_AM_I 0x0F
#define REG_CTRL1 0x20
#define REG_CTRL3 0x22
#define REG_CTRL4 0x23
#define REG_STATUS 0x27
#define REG_INT1_CFG 0x30
#define REG_INT1_THS 0x32
#define REG_INT1_DURATION 0x33
#define REG_INT1_SRC 0x31
#define REG_OUT_X_L 0x28

void MotionSensor::writeReg(uint8_t reg, uint8_t val) {
  pinMode(_sda, OUTPUT);
  pinMode(_scl, OUTPUT);
  i2cStart(_sda, _scl);
  i2cWriteByte(_sda, _scl, LIS3DH_ADDR << 1);
  i2cWriteByte(_sda, _scl, reg);
  i2cWriteByte(_sda, _scl, val);
  i2cStop(_sda, _scl);
}

uint8_t MotionSensor::readReg(uint8_t reg) {
  pinMode(_sda, OUTPUT);
  pinMode(_scl, OUTPUT);
  i2cStart(_sda, _scl);
  i2cWriteByte(_sda, _scl, LIS3DH_ADDR << 1);
  i2cWriteByte(_sda, _scl, reg);
  i2cStart(_sda, _scl);  // 重复起始，转读
  i2cWriteByte(_sda, _scl, (LIS3DH_ADDR << 1) | 1);
  uint8_t v = i2cReadByte(_sda, _scl, true);
  i2cStop(_sda, _scl);
  return v;
}

bool MotionSensor::begin(uint8_t sda, uint8_t scl, uint8_t intPin) {
  _sda = sda;
  _scl = scl;
  _intPin = intPin;
  pinMode(_sda, OUTPUT);
  pinMode(_scl, OUTPUT);
  digitalWrite(_sda, HIGH);
  digitalWrite(_scl, HIGH);

  if (readReg(REG_WHO_AM_I) != 0x33) {
    Serial.println("[motion] LIS3DH not found");
    return false;
  }
  // CTRL1: 正常模式, X/Y/Z 使能, ODR=1Hz (低功耗运动检测足够)
  writeReg(REG_CTRL1, 0x07);   // 0b00000111: XYZ on, ODR 1Hz
  // CTRL4: 量程 ±2g
  writeReg(REG_CTRL4, 0x00);
  // INT1 配置：运动检测（OR 组合 XYZ），阈值与时长
  writeReg(REG_INT1_CFG, 0x2A);    // AOI=0, 6D=0, OR: ZHIE|YHIE|XHIE
  writeReg(REG_INT1_THS, 0x20);    // 阈值 0x20/64 × 2g ≈ 0.25g
  writeReg(REG_INT1_DURATION, 0x04);  // 持续 4×ODR 周期防抖
  // CTRL3: INT1 输出使能
  writeReg(REG_CTRL3, 0x40);       // I1_IA1 使能

  pinMode(_intPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(_intPin), motionISR, RISING);
  _ok = true;
  Serial.println("[motion] LIS3DH ready");
  return true;
}

bool MotionSensor::takeMotion() {
  if (!_motion) return false;
  _motion = false;
  // 读 INT1_SRC 清中断（写 1 清零由读自动完成）
  readReg(REG_INT1_SRC);
  return true;
}
