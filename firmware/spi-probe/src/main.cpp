#include <Arduino.h>
#include <SPI.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

// ============================================================================
// spi-probe v2 —— 只做一件事：把 SPI 接口本身测清楚
//
// 设计原则（针对上一版的教训：上电就 8MHz 连发、NSS 常低、开机自动跑一串
// 自检、串口刷一屏中文 —— 示波器上必然是一片乱）：
//
//   1) 上电**什么都不发**。只停在空闲态：NSS=高、SCK=低、MOSI=低、RST=高。
//      不自动跑任何检测，不连续灌数据 —— 示波器上永远是干净基线。
//   2) 一次动作 = **一个完整帧**：NSS 拉低 → 8/16/19 个时钟 → NSS 拉高
//      → 帧间隔空档（默认 100ms）。帧内一次发出（transferBytes），
//      字节之间没有额外缝隙。
//   3) 时钟默认 **250kHz**（1 时钟 = 4µs），示波器能逐位看清；要提速按 s。
//   4) 串口输出**纯 ASCII**（没有中文 → 任何终端都不会乱码），行都很短：
//      连续模式每秒一行；逐帧日志默认关，按 l 才开。
//
// 引脚归属（唯一容易踩的坑）：
//   SPI.begin() 会把 SCK/MISO/MOSI 挂到 SPI 外设上，此后 digitalWrite 对这三根
//   脚无效 → 要位翻转（b 键）或要摆静态电平，必须先 SPI.end() 把引脚还给 GPIO。
//   NSS 一直是普通 GPIO（核心不启用硬件 CS），随时 digitalWrite 可控。
// ============================================================================

// ---------------------------------------------------------------- 状态
static uint32_t g_hz       = SPI_HZ_DEFAULT;
static uint16_t g_periodMs = FRAME_PERIOD_MS_DEFAULT;
static uint8_t  g_mode     = 0;      // 0=IDLE 1=TX1B 2=GETSTATUS 3=VERREAD
static bool     g_log      = false;  // 逐帧日志（默认关）
static bool     g_spiUp    = false;  // SPI 外设是否已接管 SCK/MISO/MOSI
static uint32_t g_frames   = 0;
static uint32_t g_busyTo   = 0;
static uint32_t g_lastTx   = 0;
static uint32_t g_lastSum  = 0;
static uint8_t  g_lastSt   = 0;
static uint8_t  g_lastVerRaw[16];
static char     g_lastVer[24] = "-";
static uint32_t g_ledUntil = 0;

// ---------------------------------------------------------------- 串口（纯 ASCII）
static void pc(const char *fmt, ...) {
  if (!(bool)Serial) return;
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.print(b);
}

static void ledOff() { digitalWrite(PIN_LED, (LED_ON_LEVEL == HIGH) ? LOW : HIGH); }
static void ledOn()  { digitalWrite(PIN_LED, LED_ON_LEVEL); }

// ---------------------------------------------------------------- 引脚
// 空闲态：NSS=H(不选中) SCK=L MOSI=L RST=H(不复位)；MISO/BUSY 只做输入
static void pinsIdle() {
  pinMode(PIN_LED, OUTPUT);  ledOff();
  pinMode(PIN_NSS, OUTPUT);  digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_SCK, OUTPUT);  digitalWrite(PIN_SCK, LOW);
  pinMode(PIN_MOSI, OUTPUT); digitalWrite(PIN_MOSI, LOW);
  pinMode(PIN_RST, OUTPUT);  digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_BUSY, INPUT);
}

// 让 SPI 外设接管 SCK/MISO/MOSI，并保证 NSS 处于不选中
static void spiUp() {
  if (!g_spiUp) {
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
    g_spiUp = true;
  }
  pinMode(PIN_NSS, OUTPUT);
  digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_BUSY, INPUT);
}

// 把引脚从 SPI 外设手里收回来（SPIClass::end() 会 detach + 设成 INPUT）
static void spiDown() {
  if (g_spiUp) {
    SPI.end();
    g_spiUp = false;
  }
  pinsIdle();
}

// 停发，回到空闲基线
static void modeIdle() {
  g_mode = 0;
  spiDown();
}

// ---------------------------------------------------------------- 发一帧
static void waitBusyLow() {
  const uint32_t t0 = micros();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if ((uint32_t)(micros() - t0) > BUSY_WAIT_US) { g_busyTo++; return; }
  }
}

// 发一帧：BUSY 等低 → NSS 低 → n 字节 → NSS 高。整个固件只有这里产生时钟。
static void frameTx(const uint8_t *tx, uint8_t *rx, uint16_t n) {
  waitBusyLow();
  SPI.beginTransaction(SPISettings(g_hz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transferBytes(tx, rx, n);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  g_frames++;
}

// 帧 1：1 字节 0xAA（8 个时钟）—— 纯波形，不看 MISO
static void frame1Byte() {
  uint8_t tx = 0xAA, rx = 0;
  frameTx(&tx, &rx, 1);
}

// 帧 2：GetStatus = 0xC0 + NOP（16 个时钟）—— 芯片应回 1 字节
static uint8_t frameGetStatus() {
  uint8_t tx[2] = {CMD_GET_STATUS, 0x00};
  uint8_t rx[2] = {0, 0};
  frameTx(tx, rx, 2);
  g_lastSt = rx[1];
  return rx[1];
}

// 帧 3：读版本寄存器 = 0x1D + 地址 0x0320 + 16 个 NOP（共 19 字节 = 152 时钟）
// MISO 上就是 ASCII 型号串（SX1268 / SX1262 / SX1261 / LLCC68）
static void frameVerRead(uint8_t out16[16]) {
  uint8_t tx[19], rx[19];
  memset(tx, 0, sizeof(tx));
  tx[0] = CMD_READ_REGISTER;
  tx[1] = (uint8_t)(REG_VERSION_STRING >> 8);
  tx[2] = (uint8_t)(REG_VERSION_STRING & 0xFF);
  frameTx(tx, rx, 19);
  memcpy(out16, rx + 3, 16);      // 命令 + 2 字节地址之后就是数据
  memcpy(g_lastVerRaw, out16, 16);
}

// ---------------------------------------------------------------- 判读
static const char *statusModeName(uint8_t st) {
  switch ((st & 0x70) >> 4) {
    case 0x2: return "STDBY_RC";
    case 0x3: return "STDBY_XOSC";
    case 0x4: return "FS";
    case 0x5: return "RX";
    case 0x6: return "TX";
    default:  return "invalid";
  }
}

static bool statusOk(uint8_t st) {
  if (st == 0x00 || st == 0xFF) return false;
  const uint8_t m = (uint8_t)((st & 0x70) >> 4);
  return (m >= 2 && m <= 6);
}

static void verToAscii(const uint8_t *v, char *out, size_t cap) {
  size_t p = 0;
  if (cap == 0) return;
  for (uint8_t i = 0; i < 16 && (p + 1) < cap; i++)
    out[p++] = (v[i] >= 0x20 && v[i] <= 0x7E) ? (char)v[i] : '.';
  out[p] = 0;
}

static bool verIsChip(const uint8_t *v) {
  static const char *const names[4] = {"SX1261", "SX1262", "SX1268", "LLCC68"};
  for (uint8_t i = 0; i < 4; i++)
    if (memcmp(v, names[i], 6) == 0) return true;
  return false;
}

static void hexBytes(const uint8_t *b, uint16_t n, char *out, size_t cap) {
  size_t p = 0;
  out[0] = 0;
  for (uint16_t i = 0; i < n && (p + 4) < cap; i++)
    p += (size_t)snprintf(out + p, cap - p, "%02X ", b[i]);
  if (p > 0) out[p - 1] = 0;
}

// ---------------------------------------------------------------- 位翻转（不经 SPI 外设）
// 用途：硬件 SPI 走不通时的对照 —— 引脚还能不能翻转、线是不是真通。
// 必须先 spiDown()，否则 SCK/MOSI 还挂在 SPI 外设上，digitalWrite 是空操作。
static void bitBangByte(uint8_t b, uint32_t halfUs) {
  for (int8_t i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, ((b >> i) & 1) ? HIGH : LOW);
    delayMicroseconds(halfUs);
    digitalWrite(PIN_SCK, HIGH);
    delayMicroseconds(halfUs);
    digitalWrite(PIN_SCK, LOW);
    delayMicroseconds(halfUs);
  }
  g_frames++;
}

// ---------------------------------------------------------------- 连续模式的三个动作
static void doFrame1() { frame1Byte(); }

static void doFrame2() {
  const uint8_t st = frameGetStatus();
  if (g_log)
    pc("#2   tx=C0 00     rx=-- %02X    status=0x%02X mode=%s %s\n", st, st,
       statusModeName(st), statusOk(st) ? "OK" : "BAD");
}

static void doFrame3() {
  uint8_t v[16];
  frameVerRead(v);
  verToAscii(v, g_lastVer, sizeof(g_lastVer));
  if (g_log) {
    char h[64];
    hexBytes(v, 16, h, sizeof(h));
    pc("#3   rx=[%s]  ascii=\"%s\"  %s\n", h, g_lastVer,
       verIsChip(v) ? "CHIP OK" : "NOT A KNOWN CHIP");
  }
}

// 连续模式：每秒一行简报（不是每帧一行）
static void summary(uint32_t now) {
  if ((uint32_t)(now - g_lastSum) < 1000) return;
  g_lastSum = now;
  if (g_mode == 2)
    pc("st=0x%02X %-10s %-3s | frames=%lu busyTo=%lu | clk=%lukHz gap=%ums\n",
       g_lastSt, statusModeName(g_lastSt), statusOk(g_lastSt) ? "OK" : "BAD",
       (unsigned long)g_frames, (unsigned long)g_busyTo,
       (unsigned long)(g_hz / 1000), (unsigned)g_periodMs);
  else if (g_mode == 3)
    pc("ver=\"%s\" %-10s | frames=%lu busyTo=%lu | clk=%lukHz gap=%ums\n",
       g_lastVer, verIsChip(g_lastVerRaw) ? "CHIP OK" : "UNKNOWN",
       (unsigned long)g_frames, (unsigned long)g_busyTo,
       (unsigned long)(g_hz / 1000), (unsigned)g_periodMs);
}

// ---------------------------------------------------------------- 提示
static const char *modeName(uint8_t m) {
  switch (m) {
    case 0: return "IDLE (no SPI activity)";
    case 1: return "loop TX 1 byte 0xAA (8 clocks)";
    case 2: return "loop TX GetStatus C0 00 (16 clocks)";
    case 3: return "loop TX ver-read 1D 03 20 +16 (19 bytes)";
    default: return "?";
  }
}

static void printIdleLine() {
  pc("levels: NSS=%u SCK=%u MOSI=%u RST=%u | MISO=%u BUSY=%u\n",
     (unsigned)digitalRead(PIN_NSS), (unsigned)digitalRead(PIN_SCK),
     (unsigned)digitalRead(PIN_MOSI), (unsigned)digitalRead(PIN_RST),
     (unsigned)digitalRead(PIN_MISO), (unsigned)digitalRead(PIN_BUSY));
}

static void printHelp() {
  pc("keys (single char, no Enter needed):\n");
  pc("  0  IDLE    stop sending; all lines static  (this is the boot state)\n");
  pc("  1  loop    1 byte 0xAA           -> 8 clocks   waveform only\n");
  pc("  2  loop    GetStatus C0 00       -> 16 clocks  chip answers 1 byte\n");
  pc("  3  loop    ver-read 1D 03 20 +16 -> 19 bytes   MISO should read \"SX1268\"\n");
  pc("  4  ONCE    one GetStatus frame   -> use scope SINGLE trigger first\n");
  pc("  5  ONCE    one ver-read frame    -> prints hex + ascii\n");
  pc("  b  bitbang 1 byte 0xAA @1kHz     -> through GPIO, NOT the SPI peripheral\n");
  pc("  s  clock   250k -> 1M -> 2M -> 8M -> 16M\n");
  pc("  g  gap     20 -> 100 -> 500 -> 2000 ms between frames\n");
  pc("  i  show    pin levels + current state\n");
  pc("  l  log     per-frame log on/off (default OFF; loop modes print 1 line/s)\n");
  pc("  r  reset   RST low 2ms then HIGH\n");
}

static void printState() {
  pc("state: mode=%u %s | clk=%lukHz gap=%ums | log=%s | frames=%lu busyTo=%lu\n",
     (unsigned)g_mode, modeName(g_mode), (unsigned long)(g_hz / 1000),
     (unsigned)g_periodMs, g_log ? "ON" : "OFF",
     (unsigned long)g_frames, (unsigned long)g_busyTo);
}

static void startLoopMode(uint8_t m) {
  spiDown();          // 先回干净基线，再让外设接管
  spiUp();
  g_mode = m;
  g_lastTx = 0;
  g_lastSum = 0;
  pc("#mode = %s\n", modeName(m));
}

static void setClockStep() {
  static const uint32_t hz[5] = {250000UL, 1000000UL, 2000000UL, 8000000UL,
                                 16000000UL};
  uint8_t i = 0;
  for (i = 0; i < 5; i++) if (hz[i] == g_hz) break;
  g_hz = hz[(i + 1) % 5];
  pc("#clk = %lukHz  (1 clock = %luns)\n", (unsigned long)(g_hz / 1000),
     (unsigned)(1000000UL / g_hz));
}

static void setGapStep() {
  static const uint16_t ms[4] = {20, 100, 500, 2000};
  uint8_t i = 0;
  for (i = 0; i < 4; i++) if (ms[i] == g_periodMs) break;
  g_periodMs = ms[(i + 1) % 4];
  pc("#gap = %ums\n", (unsigned)g_periodMs);
}

// ---------------------------------------------------------------- 串口命令
static void execCmd(char c) {
  switch (c) {
    case 'h': case '?': printHelp(); break;

    case '0':
      modeIdle();
      pc("#mode = %s\n", modeName(0));
      printIdleLine();
      break;

    case '1': startLoopMode(1); break;
    case '2': startLoopMode(2); break;
    case '3': startLoopMode(3); break;

    case '4': {
      spiDown();
      spiUp();
      const uint8_t st = frameGetStatus();
      g_ledUntil = millis() + 60;
      pc("#4 ONE GetStatus frame: tx=C0 00 | rx=-- %02X | status=0x%02X mode=%s %s\n",
         st, st, statusModeName(st), statusOk(st) ? "OK" : "BAD");
      if (!statusOk(st))
        pc("   clean NSS + 16 clocks on the scope, but this byte is 00/FF -> check MISO next\n");
      break;
    }

    case '5': {
      spiDown();
      spiUp();
      uint8_t v[16];
      frameVerRead(v);
      g_ledUntil = millis() + 60;
      char h[64];
      hexBytes(v, 16, h, sizeof(h));
      verToAscii(v, g_lastVer, sizeof(g_lastVer));
      pc("#5 ONE ver-read frame: 19 bytes / 152 clocks\n");
      pc("   rx=[%s]\n", h);
      pc("   ascii=\"%s\" %s\n", g_lastVer,
         verIsChip(v) ? "-> SPI works, chip is alive"
                      : "-> not a known SX126x version string");
      break;
    }

    case 'b':
      spiDown();                 // 关键：先把引脚从 SPI 外设手里收回来
      pc("#b bit-bang 1 byte 0xAA @1kHz (8 clocks, 500us each), SPI peripheral NOT used\n");
      delay(2);
      bitBangByte(0xAA, 500);
      g_ledUntil = millis() + 60;
      pc("#b done; NSS back HIGH\n");
      break;

    case 's': setClockStep(); break;
    case 'g': setGapStep(); break;

    case 'i':
      printState();
      printIdleLine();
      break;

    case 'l':
      g_log = !g_log;
      pc("#per-frame log = %s\n", g_log ? "ON" : "OFF");
      break;

    case 'r':
      modeIdle();
      digitalWrite(PIN_RST, LOW);
      delay(2);
      digitalWrite(PIN_RST, HIGH);
      pc("#r RST low 2ms then HIGH; BUSY now = %u\n", (unsigned)digitalRead(PIN_BUSY));
      break;

    default:
      pc("#? unknown key '%c' - press h for help\n", c);
      break;
  }
}

static void handleSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n' || c == ' ') continue;   // 单键，不用回车
    execCmd(c);
  }
}

// ---------------------------------------------------------------- LED
// 每秒短闪一次 = 固件在跑；单次发送后亮 60ms = 告诉你"刚发了一帧"
static void ledTick(uint32_t now) {
  static uint32_t next = 0;
  if ((int32_t)(now - next) >= 0) {
    next = now + 1000;
    if (g_ledUntil < now + 30) g_ledUntil = now + 30;
  }
  if ((int32_t)(now - g_ledUntil) < 0) ledOn(); else ledOff();
}

// ---------------------------------------------------------------- setup / loop
static void printBanner() {
  pc("\n===== spi-probe v2 : SPI interface test (minimal) =====\n");
  pc("board : %s\n", BOARD_NAME);
  pc("pins  : NSS=G%d SCK=G%d MOSI=G%d MISO=G%d RST=G%d BUSY=G%d\n",
     (int)PIN_NSS, (int)PIN_SCK, (int)PIN_MOSI, (int)PIN_MISO, (int)PIN_RST,
     (int)PIN_BUSY);
  pc("idle  : NSS=H SCK=L MOSI=L RST=H  -> nothing is sent until you press a key\n");
  pc("scope : trigger on NSS falling edge; one frame = 8/16/19 clocks, then a gap\n");
  pc("        clk=%lukHz (1 clock = %luns)  gap=%ums\n",
     (unsigned long)(g_hz / 1000), (unsigned)(1000000UL / g_hz),
     (unsigned)g_periodMs);
  pc("start : key 4 or 5 (ONE frame) with scope SINGLE, then key 2 for a loop\n");
}

void setup() {
  pinsIdle();
  Serial.begin(115200);
  delay(200);
  printBanner();
  printHelp();
  printIdleLine();
}

void loop() {
  const uint32_t now = millis();

  // 串口后接入（开机时没开监视器）→ 补打一次，避免对着空屏
  static bool hostSeen = false;
  if (!hostSeen && (bool)Serial) {
    hostSeen = true;
    delay(200);
    printBanner();
    printHelp();
    printState();
    printIdleLine();
  }

  handleSerial();

  switch (g_mode) {
    case 1:
      if ((uint32_t)(now - g_lastTx) >= g_periodMs) { g_lastTx = now; doFrame1(); }
      break;
    case 2:
      if ((uint32_t)(now - g_lastTx) >= g_periodMs) { g_lastTx = now; doFrame2(); }
      summary(now);
      break;
    case 3:
      if ((uint32_t)(now - g_lastTx) >= g_periodMs) { g_lastTx = now; doFrame3(); }
      summary(now);
      break;
    default:
      break;   // IDLE：什么都不发
  }

  ledTick(now);
  delay(1);
}
