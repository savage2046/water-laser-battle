#pragma once
#include <Arduino.h>

// ============================================================
// INA226 电流检测链路自检（T1）—— triger-sensor 正式板
// 覆盖：I2C 总线/器件在位、厂商与器件 ID、配置写读回、分流/总线电压读数、
//       换算电流噪声与芯片内部 Current 交叉校验、空闲基线自校准。
// ============================================================

struct Ina226SelfTestResult {
  bool run = false;        // 是否已执行
  bool pass = false;       // T1 总体结论
  bool i2cOk = false;      // 0x40 在位（I2C 应答）
  bool idOk = false;       // 厂商 ID 0x5449 / 器件 ID 0x2260
  bool cfgOk = false;      // 配置寄存器写读回一致
  bool sampleOk = false;   // 采样数据合理（量程内、噪声 σ 达标、芯片/换算电流一致）
  bool baselineOk = false; // 空闲基线采样成功
  int   scanFound = 0;     // I2C 扫描到的器件数
  uint8_t addr[8];         // 扫描到的地址（用于提示 0x40 是否被占用/错位）
  uint8_t addrN = 0;
  uint16_t manufId = 0, dieId = 0;
  float busMeanV = 0;      // 总线电压均值
  float curMeanA = 0;      // 分流换算电流均值
  float curStdevA = 0;     // 分流换算电流标准差
  float curMinA = 0, curMaxA = 0;
  float chipMeanA = 0;     // 芯片 Current 寄存器换算均值
  float idleBaselineA = 0; // 空闲基线
};

// INA226 寄存器
#define INA_REG_CFG      0x00
#define INA_REG_SHUNT    0x01   // 分流电压 2.5µV/LSB（有符号）
#define INA_REG_BUS      0x02   // 总线电压 1.25mV/LSB（无符号）
#define INA_REG_CURRENT  0x04   // 电流（LSB 由校准寄存器决定）
#define INA_REG_CAL      0x05
#define INA_REG_MANUF    0xFE   // 0x5449 "TI"
#define INA_REG_DIE      0xFF   // 0x2260

class Ina226Probe {
 public:
  // 初始化 I2C 并做 T1 全流程；打印过程行（#T1...）；结果填到 out。
  void runSelfTest(Ina226SelfTestResult &out);

  // 低层：供 T2 / 扳机监控复用
  bool begin();                    // Wire 初始化 + 器件在位检查（不打印）
  bool present();                  // I2C 0x40 应答
  uint16_t readReg(uint8_t reg);
  void writeReg(uint8_t reg, uint16_t v);
  float readBusV();                // 总线电压 V
  float readShuntV();              // 分流电压 V
  float readCurrentA();            // 分流换算电流 A（= Vshunt / Rshunt）
  float readChipCurrentA();        // 芯片 Current 寄存器换算 A
  float calibrateIdle(uint32_t ms, uint8_t nMax);  // 空闲基线 A

 private:
  bool _ok = false;
  float _chipMean = 0;
  void sampleStat(float &mean, float &stdev, float &mn, float &mx, uint32_t n, bool chipToo);
};
