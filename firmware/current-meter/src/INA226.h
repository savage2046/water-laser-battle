#pragma once
#include <Arduino.h>
#include <Wire.h>

// INA226 最小驱动（I2C，地址 0x40，分流电阻见 config.h 的 SHUNT_MOHM）
class INA226 {
 public:
  // 初始化 I2C 并配置为连续转换；返回是否读到 INA226 制造商 ID（0x5449 "TI"）
  bool begin(uint8_t sda, uint8_t scl, uint32_t clkHz = 400000);

  void writeReg(uint8_t reg, uint16_t v);
  uint16_t readReg(uint8_t reg);

  float shuntVoltageUv();  // 分流电压（µV，LSB=2.5µV）
  float busVoltageMv();    // 总线电压（mV，LSB=1.25mV）
  float currentA();        // 电流（A）= 分流电压 / 采样电阻

 private:
  uint16_t _config = 0;
};
