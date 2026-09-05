#include "CurrentSense.h"
#include "config.h"
#include <Wire.h>

// ============================================================
// INA226 电流监视器（I2C，地址 0x40，分流电阻 2mΩ）
//   寄存器：Config=0x00, Shunt=0x01, Bus=0x02, ManuID=0xFE
//   Config 0x06C7：连续 shunt+bus，AVG=1，VSHCT/VBUSCT=588µs
//                 → 转换周期 ≈1.18ms（不要用 0x00DF=AVG64，延迟过大）
// 分流电压 LSB = 2.5µV；I = V_shunt / R_shunt
// ============================================================
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
  Wire.setClock(400000);  // INA226 支持最高 2.5MHz；400kHz 下读一次 ≈75µs

  uint16_t mid = inaRead(0xFE);  // 制造商标识 0x5449 = "TI"
  if (mid != 0x5449) {
    Serial.printf("[cs] WARN INA226 not found (manuf id=0x%04X)\n", mid);
  } else {
    Serial.println("[cs] INA226 found (0x5449)");
  }
  inaWrite(0x00, 0x06C7);
#else
  pinMode(PIN_ACS712, INPUT);
  analogReadResolution(12);
#endif
}

float CurrentSense::currentA() {
#if CURRENT_SENSE_INA226
  int16_t uv = (int16_t)inaRead(0x01);       // 分流电压（2.5µV/LSB）
  float v = uv * 2.5e-6f;
  return v / ((float)SHUNT_MOHM * 1e-3f);    // A
#else
  // ACS712-20A：灵敏度 100mV/A，零点 Vcc/2（3.3V ADC 12bit）
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
  Serial.printf("[cs] idle baseline calibrated: %.3f A\n", (double)_idleOffset);
}

void CurrentSense::update(uint32_t now) {
  float i = currentA() - _idleOffset;
  _lastA = i;               // 记录本次净电流，供开火回调等读取
  bool high = i > TRIG_CURRENT_A;

  switch (_state) {
    case IDLE:
      if (high) {
        if (_hiSince == 0) _hiSince = now;
        if (now - _hiSince >= DEBOUNCE_MS) {
          _state = FIRING;
          _hiSince = now;          // 堵转计时起点
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
        _hiSince = now;   // 持续高电流保持刷新 → 堵转 = 连续高 ≥2s
        // 低→高 上升沿且距上发 ≥ PULSE_MIN_GAP_MS → 连发下一发
        if (_loSince != 0 && now - _loSince >= PULSE_MIN_GAP_MS &&
            now - _lastPulse >= PULSE_MIN_GAP_MS) {
          _lastPulse = now;
          if (onFirePulse) onFirePulse();
        }
        _loSince = 0;
        if (now - _hiSince >= STALL_MS) {
          _state = STALL;
          if (onStall) onStall();
        }
      } else {
        if (_loSince == 0) _loSince = now;
        if (now - _loSince >= END_MS) {   // 低电流持续 → 发射结束
          _state = IDLE;
          _hiSince = _loSince = 0;
          if (onFireEnd) onFireEnd();
        }
      }
      break;

    case STALL:
      // 电流回落 或 堵转持续 5s → 回空闲（等枪端恢复指令或自然回落后自恢复导通）
      if (!high || now - _hiSince >= 5000) {
        _state = IDLE;
        _hiSince = _loSince = 0;
      }
      break;
  }
}
