#pragma once
#include <Arduino.h>

// GPS 模块：UART NMEA 解析（$GPRMC / $GPGGA）。
// 输出经纬度（微度整数，度×1e6）、定位有效标志、卫星数。
// 移动超过阈值时标记 changed，供上层决定是否上报（省 915M 带宽）。
class Gps {
 public:
  void begin(uint8_t rxPin, uint32_t baud = 9600);

  // 非阻塞轮询串口，解析 NMEA 行；应在主循环频繁调用
  void update();

  bool hasFix() { return _fix; }
  uint8_t satellites() { return _sats; }
  int32_t latE6() { return _latE6; }   // 微度（北纬正）
  int32_t lonE6() { return _lonE6; }   // 微度（东经正）
  float speedKmh() { return _speedKmh; }

  // 与上次上报位置相比是否移动超过阈值（米），返回后重置基准点
  bool takeMoved();
  void setMovedThresholdM(float meters) { _threshM = meters; }

  unsigned long lastFixMs() { return _lastFixMs; }

 private:
  void parseLine(char *line);
  void parseRMC(char *f[16], int n);
  void parseGGA(char *f[16], int n);
  static int32_t parseLatLon(char *val, char hemi, bool isLat);
  void checkMoved();

  bool _fix = false;
  uint8_t _sats = 0;
  int32_t _latE6 = 0, _lonE6 = 0;
  float _speedKmh = 0;
  float _threshM = 8.0f;         // 默认 8 米
  bool _pendingReport = false;   // 距上次上报超过阈值
  int32_t _reportLatE6 = 0, _reportLonE6 = 0;  // 上次上报基准点
  unsigned long _lastFixMs = 0;

  char _line[96];
  uint8_t _len = 0;
};

extern Gps gps;
