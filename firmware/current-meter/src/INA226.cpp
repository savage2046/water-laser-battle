#include "INA226.h"
#include "config.h"

#define REG_CONFIG 0x00
#define REG_SHUNT  0x01
#define REG_BUS    0x02
#define REG_MFG_ID 0xFE
#define REG_DIE_ID 0xFF

void INA226::writeReg(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  Wire.write(v >> 8);
  Wire.write(v & 0xFF);
  Wire.endTransmission();
}

uint16_t INA226::readReg(uint8_t reg) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)INA226_ADDR, (uint8_t)2);
  return ((uint16_t)Wire.read() << 8) | Wire.read();
}

bool INA226::begin(uint8_t sda, uint8_t scl, uint32_t clkHz) {
  Wire.begin(sda, scl);
  Wire.setClock(clkHz);

  // 校验芯片在位：Manufacturer ID 寄存器 0xFE = 0x5449（"TI"）
  const uint16_t mfg = readReg(REG_MFG_ID);
  if (mfg != 0x5449) {
    Serial.printf("[ina226] NOT FOUND: mfg=0x%04X die=0x%04X (check SDA/SCL, addr 0x40)\n",
                  mfg, readReg(REG_DIE_ID));
    return false;
  }

  // Config：连续 shunt+bus（MODE=111），AVG=1（bit[10:9]=00），
  //   VBUSCT/VSHCT = INA226_CONV_CODE（bit[8:6]/bit[5:3]）
  //   默认 code=1（204µs）→ 转换周期 (204+204)µs ≈ 0.41ms
  const uint16_t conv = (uint16_t)INA226_CONV_CODE & 0x7;
  _config = (conv << 6) | (conv << 3) | 0x0007;
  writeReg(REG_CONFIG, _config);

  const uint16_t rb = readReg(REG_CONFIG);
  if (rb != _config)
    Serial.printf("[ina226] config mismatch: wrote 0x%04X read 0x%04X\n", _config, rb);
  return true;
}

float INA226::shuntVoltageUv() {
  return (float)(int16_t)readReg(REG_SHUNT) * 2.5f;   // LSB = 2.5µV
}

float INA226::busVoltageMv() {
  return (float)(int16_t)readReg(REG_BUS) * 1.25f;    // LSB = 1.25mV
}

float INA226::currentA() {
  const float rOhm = SHUNT_MOHM * 1e-3f;
  return shuntVoltageUv() * 1e-6f / rOhm;            // V_shunt / R_shunt
}
