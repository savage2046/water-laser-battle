#include "Gps.h"
#include <math.h>

Gps gps;

void Gps::begin(uint8_t rxPin, uint32_t baud) {
  Serial1.begin(baud, SERIAL_8N1, rxPin, -1);  // 仅接收
  _len = 0;
  _fix = false;
}

void Gps::update() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') {
      if (_len > 0) {
        _line[_len] = '\0';
        parseLine(_line);
      }
      _len = 0;
    } else if (_len < sizeof(_line) - 1) {
      _line[_len++] = c;
    }
  }
}

// NMEA 行切分为逗号字段（原地修改 line）
void Gps::parseLine(char *line) {
  if (line[0] != '$') return;
  // $GPRMC,...
  if (strncmp(line + 1, "GPRMC,", 6) == 0 || strncmp(line + 1, "GNRMC,", 6) == 0) {
    char *f[16];
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < 16; p++) {
      if (*p == ',') {
        *p = '\0';
        f[n++] = p + 1;
      }
    }
    parseRMC(f, n);
  } else if (strncmp(line + 1, "GPGGA,", 6) == 0 ||
             strncmp(line + 1, "GNGGA,", 6) == 0) {
    char *f[16];
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < 16; p++) {
      if (*p == ',') {
        *p = '\0';
        f[n++] = p + 1;
      }
    }
    parseGGA(f, n);
  }
}

// $GPRMC: 1=时间 2=状态(A/V) 3=纬度 4=N/S 5=经度 6=E/W 7=速度(节) 8=航向 ...
void Gps::parseRMC(char *f[16], int n) {
  if (n < 7) return;
  if (f[2][0] != 'A') {  // 状态非有效
    _fix = false;
    return;
  }
  int32_t lat = parseLatLon(f[3], f[4][0], true);
  int32_t lon = parseLatLon(f[5], f[6][0], false);
  if (lat == 0 && lon == 0) return;  // 空坐标
  float knots = n > 7 ? atof(f[7]) : 0;
  _latE6 = lat;
  _lonE6 = lon;
  _speedKmh = knots * 1.852f;
  _fix = true;
  _lastFixMs = millis();
  checkMoved();
}

// $GPGGA: 1=时间 2=纬度 3=N/S 4=经度 5=E/W 6=定位质量(0=无效) 7=卫星数 ...
void Gps::parseGGA(char *f[16], int n) {
  if (n < 7) return;
  if (f[6][0] == '0') {  // 定位质量 0 = 无效
    _fix = false;
    return;
  }
  int32_t lat = parseLatLon(f[2], f[3][0], true);
  int32_t lon = parseLatLon(f[4], f[5][0], false);
  if (lat == 0 && lon == 0) return;
  _latE6 = lat;
  _lonE6 = lon;
  _sats = n > 7 ? (uint8_t)atoi(f[7]) : 0;
  _fix = true;
  _lastFixMs = millis();
  checkMoved();
}

// NMEA 纬度/经度：ddmm.mmmm（度分格式）→ 微度整数
int32_t Gps::parseLatLon(char *val, char hemi, bool isLat) {
  if (!val || val[0] == '\0') return 0;
  // 找到度分分隔点（小数点前 2 位为度，纬度；经度 3 位为度）
  int degLen = isLat ? 2 : 3;
  char degStr[8] = "";
  strncpy(degStr, val, degLen);
  degStr[degLen] = '\0';
  const char *minPart = val + degLen;
  double deg = atof(degStr);
  double min = atof(minPart);
  double total = deg + min / 60.0;
  if (hemi == 'S' || hemi == 'W') total = -total;
  return (int32_t)llround(total * 1e6);
}

// 位置变化超过阈值（米）则置待上报标志
void Gps::checkMoved() {
  float dLatM = ((float)(_latE6 - _reportLatE6)) / 1e6f * 111320.0f;
  float dLonM = ((float)(_lonE6 - _reportLonE6)) / 1e6f * 111320.0f *
                cos((float)_latE6 / 1e6f * 3.14159265f / 180.0f);
  float dist = sqrtf(dLatM * dLatM + dLonM * dLonM);
  if (dist >= _threshM) _pendingReport = true;
}

// 上报前调用：返回 true 表示应上报，并把当前位置设为新的上报基准
bool Gps::takeMoved() {
  if (!_fix) return false;
  bool report = _pendingReport;
  _pendingReport = false;
  if (report) {
    _reportLatE6 = _latE6;
    _reportLonE6 = _lonE6;
  }
  return report;
}
