#include "Ina226Probe.h"
#include "config.h"
#include "Trace.h"
#include <Wire.h>

static float shuntOhm() { return (float)SHUNT_MOHM * 1e-3f; }

// ======================= 低层读写 =======================
bool Ina226Probe::begin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  _ok = present();
  return _ok;
}

bool Ina226Probe::present() {
  Wire.beginTransmission(INA226_ADDR);
  return Wire.endTransmission() == 0;
}

uint16_t Ina226Probe::readReg(uint8_t reg) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)INA226_ADDR, (uint8_t)2);
  if (Wire.available() < 2) return 0xFFFF;
  uint16_t v = ((uint16_t)Wire.read() << 8) | Wire.read();
  return v;
}

void Ina226Probe::writeReg(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  Wire.write(v >> 8);
  Wire.write(v & 0xFF);
  Wire.endTransmission();
}

float Ina226Probe::readBusV() {
  return readReg(INA_REG_BUS) * 1.25e-3f;      // 1.25mV/LSB
}

float Ina226Probe::readShuntV() {
  return (int16_t)readReg(INA_REG_SHUNT) * 2.5e-6f;  // 2.5µV/LSB，有符号
}

float Ina226Probe::readCurrentA() {
  return readShuntV() / shuntOhm();            // A（分流换算，方向随接线）
}

float Ina226Probe::readChipCurrentA() {
  return (int16_t)readReg(INA_REG_CURRENT) * 1e-3f;  // CAL=0x0A00 → LSB=1mA
}

// ======================= 统计采样 =======================
void Ina226Probe::sampleStat(float &mean, float &stdev, float &mn, float &mx,
                             uint32_t n, bool chipToo) {
  mean = stdev = 0;
  mn = 1e9f; mx = -1e9f;
  float chipSum = 0;
  if (chipToo) {
    // 采样前先丢弃一次读数（配置切换后的首次转换可能不完整）
    readCurrentA();
  }
  for (uint32_t i = 0; i < n; i++) {
    float v = readCurrentA();
    if (chipToo) chipSum += readChipCurrentA();
    mean += v;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    delay(CS_SAMPLE_GAP_MS);
  }
  mean /= (float)n;
  for (uint32_t i = 0; i < n; i++) {           // 重采一遍求 σ（读数稳定，代价低）
    float v = readCurrentA();
    float d = v - mean;
    stdev += d * d;
    delay(CS_SAMPLE_GAP_MS);
  }
  stdev = sqrtf(stdev / (float)n);
  if (chipToo) _chipMean = chipSum / (float)n; // 存成员供 runSelfTest 取用
}

// ======================= 空闲基线 =======================
float Ina226Probe::calibrateIdle(uint32_t ms, uint8_t nMax) {
  uint32_t t0 = millis();
  float sum = 0;
  uint32_t n = 0;
  while (millis() - t0 < ms && n < nMax) {
    sum += readCurrentA();
    n++;
    delay(CS_SAMPLE_GAP_MS);
  }
  return n ? sum / (float)n : 0;
}

// ======================= T1 全流程 =======================
void Ina226Probe::runSelfTest(Ina226SelfTestResult &out) {
  out.run = true;
  DBG("\n#T1 ===== [1] 电流检测链路自检（INA226 @0x40，2mΩ 分流）=====\n");

  // 1) I2C 扫描 + 0x40 在位
  out.addrN = 0;
  for (uint16_t a = 0x03; a <= 0x77; a++) {
    Wire.beginTransmission((uint8_t)a);
    if (Wire.endTransmission() == 0) {
      if (out.addrN < 8) out.addr[out.addrN++] = (uint8_t)a;
    }
  }
  out.scanFound = out.addrN;
  out.i2cOk = false;
  for (uint8_t i = 0; i < out.addrN; i++)
    if (out.addr[i] == INA226_ADDR) out.i2cOk = true;
  DBG("#T1 i2c_scan=%d addr=", out.scanFound);
  for (uint8_t i = 0; i < out.addrN; i++)
    DBG("%s0x%02X", i ? "," : "", out.addr[i]);
  DBG(" -> 0x40 %s\n", out.i2cOk ? "OK" : "缺失");
  if (!out.i2cOk) {
    DBG("#T1 reason=INA226 无应答：查 U3 焊接/虚焊、R2/R3(2.2k) 上拉、VS+ 3.3V 供电、"
        "SDA=IO4/SCL=IO5 通路\n");
    out.pass = false;
    return;
  }

  // 2) 厂商/器件 ID
  out.manufId = readReg(INA_REG_MANUF);
  out.dieId = readReg(INA_REG_DIE);
  out.idOk = (out.manufId == 0x5449) && (out.dieId == 0x2260);
  DBG("#T1 id=manuf 0x%04X die 0x%04X -> %s\n", out.manufId, out.dieId,
      out.idOk ? "INA226 确认" : "ID 不符(非 INA226?)");

  // 3) 配置写读回（含校准寄存器）
  writeReg(INA_REG_CFG, INA_CFG);
  delay(5);
  uint16_t cfg = readReg(INA_REG_CFG);
  writeReg(INA_REG_CAL, INA_CAL);
  delay(2);
  uint16_t cal = readReg(INA_REG_CAL);
  out.cfgOk = (cfg == INA_CFG) && (cal == INA_CAL);
  DBG("#T1 cfg=0x%04X(%s) cal=0x%04X(%s) -> %s\n", cfg,
      cfg == INA_CFG ? "写读回一致" : "不一致", cal,
      cal == INA_CAL ? "写读回一致" : "不一致", out.cfgOk ? "OK" : "FAIL");

  // 4) 采样统计：分流换算电流（含噪声/极值）+ 芯片 Current 交叉校验 + 总线电压
  float mn = 0, mx = 0;
  out.chipMeanA = 0;
  sampleStat(out.curMeanA, out.curStdevA, mn, mx, CS_SAMPLES, true);
  out.chipMeanA = _chipMean;
  out.curMinA = mn;
  out.curMaxA = mx;

  float busSum = 0;
  for (uint16_t i = 0; i < CS_SAMPLES; i++) {   // 总线电压单独再采一轮（读数慢些）
    busSum += readBusV();
    delay(CS_SAMPLE_GAP_MS);
  }
  out.busMeanV = busSum / (float)CS_SAMPLES;

  float chipDiff = fabsf(out.curMeanA - out.chipMeanA);
  bool inRange = fabsf(out.curMeanA) <= CS_CUR_RANGE_MAX_A &&
                 fabsf(out.chipMeanA) <= CS_CUR_RANGE_MAX_A &&
                 fabsf(mn) <= CS_CUR_RANGE_MAX_A && fabsf(mx) <= CS_CUR_RANGE_MAX_A;
  out.sampleOk = inRange &&
                 out.curStdevA <= CS_CUR_STDEV_MAX_A &&
                 chipDiff <= CS_CHIP_DIFF_MAX_A;

  DBG("#T1 sample n=%d bus=%.3fV cur_mean=%.4fA cur_stdev=%.4fA cur_min=%.4fA "
      "cur_max=%.4fA chip_mean=%.4fA chip_diff=%.4fA -> %s\n",
      (int)CS_SAMPLES, (double)out.busMeanV, (double)out.curMeanA,
      (double)out.curStdevA, (double)out.curMinA, (double)out.curMaxA,
      (double)out.chipMeanA, (double)chipDiff, out.sampleOk ? "OK" : "FAIL");
  if (out.busMeanV < 2.0f)
    DBG("#T1 note=VBUS≈%.2fV：当前无电池电压（USB 供电或电池未接属正常）；"
        "接 2S/3S 电池应≈7.4~12.6V\n", (double)out.busMeanV);
  if (!out.sampleOk) {
    DBG("#T1 reason=读数异常：查 R1(2mΩ) 焊接/阻值、分流采样走线、U3 周边、"
        "共地；stdev>%.2fA 或超量程多为硬件问题\n", (double)CS_CUR_STDEV_MAX_A);
  }

  // 5) 空闲基线（扣除板载自耗，供阈值判定/监控参考）
  out.idleBaselineA = calibrateIdle(300, CS_CAL_SAMPLES);
  out.baselineOk = fabsf(out.idleBaselineA) <= CS_CUR_RANGE_MAX_A;
  DBG("#T1 idle_baseline=%.4fA\n", (double)out.idleBaselineA);

  out.pass = out.i2cOk && out.idOk && out.cfgOk && out.sampleOk && out.baselineOk;
  DBG("#T1 result=%s\n", out.pass ? "PASS" : "FAIL");
}
