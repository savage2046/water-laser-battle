// ============================================================
// current-meter：电流 1Hz 简易测试固件（ESP32-S3 + INA226）
//
// 单独测试程序：只做一件事——每隔 1 秒读取一次电流，
// 通过串口输出一个电流值（单位 A），用于电流检测链路
// （INA226 → I2C → 串口）的初步验证。
//
// 用法：
//   cd firmware/current-meter
//   pio run -t upload
//   pio device monitor        # 115200
// ============================================================
#include <Arduino.h>
#include "config.h"
#include "INA226.h"

INA226 ina;

// 每次输出前对 AVG_SAMPLES 个采样取均值（INA226 转换周期 ≈0.41ms，间隔 1ms 足够）
static float readAvgCurrentA() {
  float sum = 0;
  for (int i = 0; i < AVG_SAMPLES; i++) {
    sum += ina.currentA();
    delay(1);
  }
  return sum / AVG_SAMPLES;
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // 上电拉高 G10：使电机回路 MOSFET 导通（HIGH=导通），电流才能流过采样电阻
  pinMode(PIN_MOSFET, OUTPUT);
  digitalWrite(PIN_MOSFET, HIGH);
  Serial.println("#mosfet ON (G10 HIGH)");

  Serial.println();
  Serial.println("===== current-meter : INA226 1Hz current readout =====");

  const bool ok = ina.begin(PIN_SDA, PIN_SCL, I2C_CLK_HZ);
  if (!ok)
    Serial.println("#ERROR INA226 not detected (check SDA/SCL wiring & addr 0x40)");

  Serial.printf("#shunt=%d mOhm  vbus=%.0f mV\n",
                SHUNT_MOHM, (double)ina.busVoltageMv());
  Serial.printf("#output: one current value every %d ms\n", OUTPUT_INTERVAL_MS);
  Serial.println("#format: t_ms,I_A");
}

void loop() {
  static uint32_t lastMs = 0;
  static uint32_t tMs = 0;
  const uint32_t now = millis();

  if (now - lastMs >= OUTPUT_INTERVAL_MS) {
    lastMs = now;
    tMs += OUTPUT_INTERVAL_MS;

    const float iA = readAvgCurrentA();

    digitalWrite(PIN_LED, !digitalRead(PIN_LED));  // 每秒翻转一次状态 LED

    // 每行一个电流值：t_ms,电流_A
    Serial.printf("%lu,%.3f A\n",
                  (unsigned long)tMs, (double)iA);
  }
}
