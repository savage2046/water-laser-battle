#include "CurrentSense.h"
#include "config.h"
#include <Wire.h>

// ===== INA226（I2C，分流 2mΩ）=====
// 寄存器：Config=0x00, Shunt=0x01, Bus=0x02
// Config 0x00DF：连续 shunt+bus 测量，AVG=1x，VSHCT/VBUSCT=588µs
static void inaWrite(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  Wire.write(v >> 8);
  Wire.write(v & 0xFF);
  Wire.endTransmission();
}
static uint16_t inaRead(uint8_t reg) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)INA226_ADDR, (uint8_t)2);
  uint16_t v = ((uint16_t)Wire.read() << 8) | Wire.read();
  return v;
}

void CurrentSense::begin() {
#if CURRENT_SENSE_INA226
  Wire.begin(PIN_SDA, PIN_SCL);
  // 校验芯片在位（读 Manufacturer ID 0xFE = 0x5449 "TI"）
  uint16_t mid = inaRead(0xFE);
  if (mid != 0x5449) {
    Serial.printf("[cs] INA226 not found (mid=0x%04X)\n", mid);
  }
  inaWrite(0x00, 0x00DF);  // 连续 shunt+bus，588µs×2 ≈ 1.2ms/周期
#else
  pinMode(PIN_ACS712, INPUT);
  analogReadResolution(12);
#endif
}

float CurrentSense::currentA() {
#if CURRENT_SENSE_INA226
  // 分流电压 LSB = 2.5µV；电流 = V_shunt / R_shunt
  int16_t uv = (int16_t)inaRead(0x01);       // shunt 电压（2.5µV/LSB）
  float v = uv * 2.5e-6f;
  return v / ((float)SHUNT_MOHM * 1e-3f);    // A
#else
  // ACS712-20A：灵敏度 100mV/A，零点 Vcc/2
  int adc = analogRead(PIN_ACS712);
  float v = (adc - 2048) * (3.3f / 4096.0f);
  return v / 0.1f;
#endif
}

void CurrentSense::calibrateIdle(uint32_t ms) {
  uint32_t t0 = millis();
  float sum = 0;
  uint32_t n = 0;
  while (millis() - t0 < ms) {
    sum += currentA();
    n++;
    delay(SENSE_INTERVAL_MS);
  }
  _idleOffset = n ? sum / n : 0;
  Serial.printf("[cs] idle baseline %.2fA\n", (double)_idleOffset);
}

void CurrentSense::update(uint32_t now) {
  float i = currentA() - _idleOffset;
  bool high = i > TRIG_CURRENT_A;

  switch (_state) {
    case IDLE:
      if (high) {
        if (_hiSince == 0) _hiSince = now;
        if (now - _hiSince >= DEBOUNCE_MS) {
          _state = FIRING;
          _hiSince = now;
          _loSince = 0;
          _lastPulse = now;
          if (onFirePulse) onFirePulse();   // 第一发
        }
      } else {
        _hiSince = 0;
      }
      break;

    case FIRING:
      if (high) {
        _hiSince = now;   // 刷新持续高电流计时：堵转 = 连续高 2s 无回落
        // 上升沿 → 连发下一发（防抖间隔后）
        if (_loSince != 0 && now - _loSince >= PULSE_MIN_GAP_MS &&
            now - _lastPulse >= PULSE_MIN_GAP_MS) {
          _lastPulse = now;
          if (onFirePulse) onFirePulse();
        }
        _loSince = 0;
        // 持续高电流 = 堵转
        if (now - _hiSince >= STALL_MS) {
          _state = STALL;
          if (onStall) onStall();
        }
      } else {
        if (_hiSince == 0) _hiSince = 0;   // 保持
        if (_loSince == 0) _loSince = now;
        if (now - _loSince >= END_MS) {
          _state = IDLE;
          _hiSince = _loSince = 0;
          if (onFireEnd) onFireEnd();
        }
      }
      break;

    case STALL:
      // 电流回落或超时 5s → 回到空闲（等待枪端恢复指令或电流正常）
      if (!high || now - _hiSince >= 5000) {
        _state = IDLE;
        _hiSince = _loSince = 0;
      }
      break;
  }
}
