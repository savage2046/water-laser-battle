#include "Imu9Axis.h"
#include <math.h>

Imu9Axis imu;

// ---- 软件 I2C 位操作（与 MotionSensor 相同引脚协议，地址参数化）----
static void i2cStart(uint8_t sda, uint8_t scl) {
  pinMode(sda, OUTPUT);
  pinMode(scl, OUTPUT);
  digitalWrite(sda, HIGH);
  digitalWrite(scl, HIGH);
  delayMicroseconds(5);
  digitalWrite(sda, LOW);
  delayMicroseconds(5);
  digitalWrite(scl, LOW);
}
static void i2cStop(uint8_t sda, uint8_t scl) {
  pinMode(sda, OUTPUT);
  digitalWrite(sda, LOW);
  digitalWrite(scl, HIGH);
  delayMicroseconds(5);
  digitalWrite(sda, HIGH);
  delayMicroseconds(5);
}
static bool i2cWriteByte(uint8_t sda, uint8_t scl, uint8_t v) {
  pinMode(sda, OUTPUT);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(sda, (v >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(scl, HIGH);
    delayMicroseconds(4);
    digitalWrite(scl, LOW);
    delayMicroseconds(2);
  }
  pinMode(sda, INPUT);
  delayMicroseconds(2);
  digitalWrite(scl, HIGH);
  delayMicroseconds(4);
  bool ack = digitalRead(sda) == LOW;
  digitalWrite(scl, LOW);
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
  pinMode(sda, OUTPUT);
  digitalWrite(sda, last ? HIGH : LOW);
  delayMicroseconds(2);
  digitalWrite(scl, HIGH);
  delayMicroseconds(4);
  digitalWrite(scl, LOW);
  pinMode(sda, INPUT);
  return v;
}

#define MPU_ADDR 0x68
#define MAG_ADDR 0x0C            // AK8963（MPU9250 内置磁力计）
#define REG_WHO_AM_I 0x75
#define REG_PWR_MGMT_1 0x6B
#define REG_ACCEL_XOUT_H 0x3B
#define REG_ACCEL_CONFIG 0x1C
#define MAG_WHO_AM_I 0x00
#define MAG_CNTL1 0x0A
#define MAG_HXL 0x03

void Imu9Axis::writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  i2cStart(_sda, _scl);
  i2cWriteByte(_sda, _scl, addr << 1);
  i2cWriteByte(_sda, _scl, reg);
  i2cWriteByte(_sda, _scl, val);
  i2cStop(_sda, _scl);
}

uint8_t Imu9Axis::readReg(uint8_t addr, uint8_t reg) {
  i2cStart(_sda, _scl);
  i2cWriteByte(_sda, _scl, addr << 1);
  i2cWriteByte(_sda, _scl, reg);
  i2cStart(_sda, _scl);
  i2cWriteByte(_sda, _scl, (addr << 1) | 1);
  uint8_t v = i2cReadByte(_sda, _scl, true);
  i2cStop(_sda, _scl);
  return v;
}

void Imu9Axis::readBuf(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  i2cStart(_sda, _scl);
  i2cWriteByte(_sda, _scl, addr << 1);
  i2cWriteByte(_sda, _scl, reg);
  i2cStart(_sda, _scl);
  i2cWriteByte(_sda, _scl, (addr << 1) | 1);
  for (uint8_t i = 0; i < n; i++) {
    buf[i] = i2cReadByte(_sda, _scl, i == n - 1);
  }
  i2cStop(_sda, _scl);
}

void Imu9Axis::begin(uint8_t sda, uint8_t scl, uint8_t pwrPin) {
  _sda = sda;
  _scl = scl;
  _pwrPin = pwrPin;
  if (_pwrPin != 0xFF) {
    pinMode(_pwrPin, OUTPUT);
    digitalWrite(_pwrPin, HIGH);  // 默认断电（P-MOS 高=断）
  }
  _ok = false;
}

bool Imu9Axis::powerOn() {
  if (_pwrPin != 0xFF) {
    digitalWrite(_pwrPin, LOW);   // 上电
    delay(50);                    // 稳定
  }
  _powered = true;

  if (readReg(MPU_ADDR, REG_WHO_AM_I) != 0x71) {
    Serial.println("[imu] MPU9250 not found");
    _ok = false;
    return false;
  }
  // 唤醒 + 加速度计 ±2g
  writeReg(MPU_ADDR, REG_PWR_MGMT_1, 0x00);
  writeReg(MPU_ADDR, REG_ACCEL_CONFIG, 0x00);
  // 磁力计：连续测量模式 100Hz
  writeReg(MAG_ADDR, MAG_CNTL1, 0x16);
  delay(10);
  _ok = true;
  return true;
}

void Imu9Axis::powerOff() {
  if (_pwrPin != 0xFF) {
    digitalWrite(_pwrPin, HIGH);
  }
  _powered = false;
}

void Imu9Axis::readOrientation(int16_t &yaw, int16_t &pitch, int16_t &roll) {
  uint8_t buf[6];
  // 加速度计
  readBuf(MPU_ADDR, REG_ACCEL_XOUT_H, buf, 6);
  _ax = (int16_t)((buf[0] << 8) | buf[1]);
  _ay = (int16_t)((buf[2] << 8) | buf[3]);
  _az = (int16_t)((buf[4] << 8) | buf[5]);
  // 磁力计（AK8963 输出 16bit LE）
  readBuf(MAG_ADDR, MAG_HXL, buf, 6);
  _mx = (int16_t)(buf[0] | (buf[1] << 8));
  _my = (int16_t)(buf[2] | (buf[3] << 8));
  _mz = (int16_t)(buf[4] | (buf[5] << 8));

  // 加速度归一化（±2g → 16384 LSB/g）
  float ax = _ax / 16384.0f, ay = _ay / 16384.0f, az = _az / 16384.0f;

  // pitch/roll 从重力分量
  float pitchR = atan2f(-ax, sqrtf(ay * ay + az * az));
  float rollR = atan2f(ay, az);
  pitch = (int16_t)roundf(pitchR * 180.0f / M_PI);
  roll = (int16_t)roundf(rollR * 180.0f / M_PI);

  // yaw：磁力计投影到水平面（用 pitch/roll 校正）
  float sp = sinf(pitchR), cp = cosf(pitchR);
  float sr = sinf(rollR), cr = cosf(rollR);
  float my = _my, mx = _mx, mz = _mz;
  // 水平面投影（标准公式）
  float mxh = mx * cp + my * sp * sr + mz * sp * cr;
  float myh = my * cr - mz * sr;
  float yawR = atan2f(myh, mxh);
  yaw = (int16_t)(((int)roundf(yawR * 180.0f / M_PI) + 360) % 360);
}
