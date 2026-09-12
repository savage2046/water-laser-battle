#include <Arduino.h>
#include <SPI.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

// ============================================================================
// spi-read —— 第二步：通过 SPI 读 LoRa(SX126x) 的寄存器/参数，打印到串口
//
// ⚠️ 关键：SX126x 返回**数据**的命令，在命令/地址之后先吐一个 Status 字节，
//    数据从下一个字节才开始（手册的 SPI 交互表原文）：
//
//      表 13-25 READREGISTER   byte0=0x1D 1=addrH 2=addrL 3=NOP→**Status** 4..=data
//      表 13-39 GETPACKETTYPE  byte0=0x11 1=NOP→**Status** 2=NOP→packetType
//      表 13-84 GETDEVICEERRORS byte0=0x17 1=NOP→**Status** 2-3=NOP→OpError(15:0)
//      表 13-82 GETRSSIINST    byte0=0x15 1=NOP→**Status** 2=NOP→RssiInst
//      表 13-78 GETSTATUS      byte0=0xC0 1=NOP→**Status**      ← 这条的"数据"就是 Status
//
//    正文原话："主机必须在发送两字节的地址后发送一个 NOP，然后可以开始进行
//    下一个 NOP 触发的数据接收。"
//    → 所以读寄存器要发 n+1 个 NOP，数据从 **rx[4]** 开始取（rx[3] 是 Status）。
//    RadioLib 也是这么做的：Module.cpp:402 `memcpy(dataIn, &buffIn[cmdLen + STATUS/8], numBytes)`
//
//   串口命令：a=立刻打印一次  l=自动打印开/关  s=SPI 时钟换档
//             <地址>      例如 0740        -> 读该地址 1 字节（并打印原始字节）
//             <地址>.<n>  例如 0320.16     -> 读 n 字节
//             h=帮助
// ============================================================================

static uint32_t g_hz = SPI_HZ_DEFAULT;
static bool     g_auto = true;
static uint32_t g_busyTo = 0;      // BUSY 等待超时次数
static uint32_t g_n = 0;           // 报告序号
static uint32_t g_lastReport = 0;

// ---------------------------------------------------------------- 串口（纯 ASCII）
static void pc(const char *fmt, ...) {
  if (!(bool)Serial) return;
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.print(b);
}

// ---------------------------------------------------------------- 引脚
static void pinsSetup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, (LED_ON_LEVEL == HIGH) ? LOW : HIGH);

  pinMode(PIN_NSS, OUTPUT);
  digitalWrite(PIN_NSS, HIGH);          // NSS 低有效：空闲必须高
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, HIGH);          // RST 低有效：空闲必须高（模组不复位）
  pinMode(PIN_BUSY, INPUT);             // 模组驱动，只读
  pinMode(PIN_MISO, INPUT);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);   // 核心不会把 NSS 挂成硬件 CS
}

// ---------------------------------------------------------------- 底层
static bool busyWait() {
  const uint32_t t0 = micros();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if ((uint32_t)(micros() - t0) > BUSY_WAIT_US) {
      g_busyTo++;
      return false;
    }
  }
  return true;
}

// 一次完整事务：BUSY 等低 -> NSS 低 -> 收发 n 字节 -> NSS 高
static void xfer(const uint8_t *tx, uint8_t *rx, size_t n) {
  busyWait();
  SPI.beginTransaction(SPISettings(g_hz, MSBFIRST, SPI_MODE0));   // SX126x: CPOL=0 CPHA=0
  digitalWrite(PIN_NSS, LOW);
  SPI.transferBytes(tx, rx, (uint32_t)n);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
}

static uint8_t cmdStatus() {                    // GetStatus 0xC0 + NOP -> rx[1] = Status
  uint8_t tx[2] = {CMD_GET_STATUS, 0x00}, rx[2] = {0, 0};
  xfer(tx, rx, 2);
  return rx[1];
}

static uint8_t cmdPacketType() {                // GetPacketType 0x11 + NOP + NOP -> rx[2]
  uint8_t tx[3] = {CMD_GET_PACKET_TYPE, 0x00, 0x00}, rx[3] = {0, 0, 0};
  xfer(tx, rx, 3);
  return rx[2];
}

static uint16_t cmdDeviceErrors() {             // GetDeviceErrors 0x17 + NOP + NOP -> rx[2..3]
  uint8_t tx[4] = {CMD_GET_DEVICE_ERRORS, 0x00, 0x00, 0x00}, rx[4] = {0, 0, 0, 0};
  xfer(tx, rx, 4);
  return (uint16_t)(((uint16_t)rx[2] << 8) | rx[3]);
}

// ReadRegister 0x1D + addrH + addrL + (n+1)×NOP；rx[3] 是 Status，数据从 rx[4] 开始
// raw（可选）回传整个接收缓冲，便于核对偏移
static void cmdReadRegsRaw(uint16_t addr, uint8_t *out, uint8_t n, uint8_t *raw) {
  uint8_t tx[20], rx[20];
  if (n > 16) n = 16;
  memset(tx, 0, sizeof(tx));
  memset(rx, 0, sizeof(rx));
  tx[0] = CMD_READ_REGISTER;
  tx[1] = (uint8_t)(addr >> 8);
  tx[2] = (uint8_t)(addr & 0xFF);
  xfer(tx, rx, (size_t)n + 4);
  memcpy(out, rx + 4, n);
  if (raw) memcpy(raw, rx, (size_t)n + 4);
}

static void cmdReadRegs(uint16_t addr, uint8_t *out, uint8_t n) {
  cmdReadRegsRaw(addr, out, n, nullptr);
}

static uint8_t readReg1(uint16_t addr) {
  uint8_t v = 0;
  cmdReadRegs(addr, &v, 1);
  return v;
}

// ---------------------------------------------------------------- 判读
static const char *modeName(uint8_t st) {
  switch ((st & 0x70) >> 4) {
    case 0x2: return "STBY_RC";
    case 0x3: return "STBY_XOSC";
    case 0x4: return "FS";
    case 0x5: return "RX";
    case 0x6: return "TX";
    default:  return "?";
  }
}

static const char *pktName(uint8_t p) { return (p == 0x01) ? "LoRa" : (p == 0x00 ? "FSK" : "?"); }

static bool verIsChip(const uint8_t *v) {
  static const char *const names[4] = {"SX1261", "SX1262", "SX1268", "LLCC68"};
  for (uint8_t i = 0; i < 4; i++)
    if (memcmp(v, names[i], 6) == 0) return true;
  return false;
}

static const char *verdictOf(const uint8_t *ver, bool chip) {
  if (chip) return "OK (SPI works, chip answered)";
  uint8_t ones = 0, zeros = 0;
  for (uint8_t i = 0; i < 16; i++) {
    if (ver[i] == 0xFF) ones++;
    if (ver[i] == 0x00) zeros++;
  }
  if (ones == 16) return "MISO stuck HIGH (all FF) - check MISO wire / power / NSS";
  if (zeros == 16) return "MISO stuck LOW (all 00) - check MISO short / NSS / reset";
  return "garbage - wiring or SPI clock too fast";
}

// ---------------------------------------------------------------- 报告
static void report() {
  g_n++;
  uint8_t ver[16];
  char asc[20];
  memset(ver, 0, sizeof(ver));
  cmdReadRegs(REG_VERSION_STRING, ver, 16);
  for (uint8_t i = 0; i < 16; i++)
    asc[i] = (ver[i] >= 0x20 && ver[i] <= 0x7E) ? (char)ver[i] : '.';
  asc[16] = 0;

  const uint8_t st = cmdStatus();
  const uint8_t pkt = cmdPacketType();
  const uint16_t err = cmdDeviceErrors();
  const uint8_t syncM = readReg1(REG_LORA_SYNC_MSB);
  const uint8_t syncL = readReg1(REG_LORA_SYNC_LSB);
  const uint8_t gain = readReg1(REG_RX_GAIN);
  const uint8_t ocp = readReg1(REG_OCP);
  const uint8_t xta = readReg1(REG_XTA_TRIM);
  const uint8_t xtb = readReg1(REG_XTB_TRIM);
  uint8_t rnd[4] = {0, 0, 0, 0};
  cmdReadRegs(REG_RANDOM_GEN, rnd, 4);

  const bool chip = verIsChip(ver);
  pc("#%lu @%lukHz  st=0x%02X(%s)  pkt=0x%02X(%s)%s  err=0x%04X  busyTo=%lu\n",
     (unsigned long)g_n, (unsigned long)(g_hz / 1000), (unsigned)st, modeName(st),
     (unsigned)pkt, pktName(pkt), (pkt == 0x00 ? " [default after reset]" : ""),
     (unsigned)err, (unsigned long)g_busyTo);
  pc("   ver   0x0320 = \"%s\"   %s\n", asc, verdictOf(ver, chip));
  pc("   sync  0x0740=%02X(exp 14)%s  0x0741=%02X(exp 24)%s  -> 0x%02X%02X (%s)\n",
     (unsigned)syncM, syncM == 0x14 ? " OK" : " MISMATCH", (unsigned)syncL,
     syncL == 0x24 ? " OK" : " MISMATCH", (unsigned)syncM, (unsigned)syncL,
     (syncM == 0x14 && syncL == 0x24) ? "private net, LoRa syncword 0x12"
                                      : ((syncM == 0x34 && syncL == 0x44) ? "public net (LoRaWAN)"
                                                                          : "custom"));
  pc("   gain  0x08AC=%02X(exp 94)%s  xta 0x0911=%02X(exp 05)%s  xtb 0x0912=%02X(exp 05)%s"
     "  ocp 0x08E7=%02X\n",
     (unsigned)gain, gain == 0x94 ? " OK" : " diff", (unsigned)xta, xta == 0x05 ? " OK" : " diff",
     (unsigned)xtb, xtb == 0x05 ? " OK" : " diff", (unsigned)ocp);
  pc("   rnd   0x0819..1C = %02X %02X %02X %02X  (proves reads are live data, not a stuck bus)\n",
     rnd[0], rnd[1], rnd[2], rnd[3]);
}

// ---------------------------------------------------------------- 读任意地址
// 输入形如 "0740" 或 "0320.16"
static void readAddrCmd(const char *s) {
  char buf[24];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;

  uint8_t len = 1;
  char *dot = strchr(buf, '.');
  if (dot) {
    *dot = 0;
    const long l = atol(dot + 1);
    if (l < 1) len = 1; else if (l > 16) len = 16; else len = (uint8_t)l;
  }
  const long addr = strtol(buf, NULL, 16);
  if (addr < 0 || addr > 0xFFFF) {
    pc("#? bad address \"%s\" (use hex, e.g. 0740 or 0320.16)\n", s);
    return;
  }

  uint8_t v[16], raw[20];
  memset(v, 0, sizeof(v));
  memset(raw, 0, sizeof(raw));
  cmdReadRegsRaw((uint16_t)addr, v, len, raw);
  pc("#read 0x%04X x%u =", (unsigned)addr, (unsigned)len);
  for (uint8_t i = 0; i < len; i++) pc(" %02X", v[i]);
  pc("   (");
  for (uint8_t i = 0; i < len; i++) pc("%c", (v[i] >= 0x20 && v[i] <= 0x7E) ? v[i] : '.');
  pc(")\n");
  pc("#  raw rx: cmd=%02X addr=%02X%02X status=%02X | data=", raw[0], raw[1], raw[2], raw[3]);
  for (uint8_t i = 0; i < len; i++) pc(" %02X", raw[4 + i]);
  pc("\n");
}

// ---------------------------------------------------------------- 帮助
static void printHelp() {
  pc("keys / commands (line ends with Enter):\n");
  pc("  a          print one report now\n");
  pc("  l          auto report on/off (every %d ms)\n", REPORT_MS);
  pc("  s          SPI clock: 1M -> 2M -> 4M -> 8M -> 16M MHz\n");
  pc("  0740       read 1 byte at hex address 0x0740\n");
  pc("  0320.16    read 16 bytes at 0x0320 (version string)\n");
  pc("  h          help\n");
}

// ---------------------------------------------------------------- 命令
static void stepClock() {
  static const uint32_t hz[5] = {1000000UL, 2000000UL, 4000000UL, 8000000UL, 16000000UL};
  uint8_t i = 0;
  for (i = 0; i < 5; i++) if (hz[i] == g_hz) break;
  g_hz = hz[(i + 1) % 5];
  pc("#clk = %lukHz (the real SX126x limit is 16MHz; lower it if reads go bad)\n",
     (unsigned long)(g_hz / 1000));
}

static char s_line[32];
static uint8_t s_len = 0;

static void onLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;
  const char c = line[0];
  if (c == 'a') { report(); return; }
  if (c == 'l') { g_auto = !g_auto; pc("#auto report = %s\n", g_auto ? "ON" : "OFF"); return; }
  if (c == 's') { stepClock(); return; }
  if (c == 'h' || c == '?') { printHelp(); return; }
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
    readAddrCmd(line);
    return;
  }
  pc("#? unknown \"%s\" (h for help)\n", line);
}

static void pollSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (s_len) {
        s_line[s_len] = 0;
        s_len = 0;
        onLine(s_line);
      }
      continue;
    }
    if (c == 8 || c == 127) { if (s_len) s_len--; continue; }
    if (s_len < sizeof(s_line) - 1) s_line[s_len++] = c;
  }
}

// ---------------------------------------------------------------- setup / loop
static void banner() {
  pc("\n===== spi-read : read SX126x registers over SPI =====\n");
  pc("board : %s\n", BOARD_NAME);
  pc("spi   : NSS=G%d SCK=G%d MOSI=G%d MISO=G%d  mode0  %lukHz\n", (int)PIN_NSS,
     (int)PIN_SCK, (int)PIN_MOSI, (int)PIN_MISO, (unsigned long)(g_hz / 1000));
  pc("ctrl  : RST=G%d (driven HIGH)  BUSY=G%d (input, waited before every command)\n",
     (int)PIN_RST, (int)PIN_BUSY);
  pc("after reset the chip is in STDBY_RC and the registers hold their datasheet\n");
  pc("reset values (Table 12-1): 0x0740=0x14 0x0741=0x24 0x08AC=0x94 0x0911=0x05 0x0912=0x05\n");
  printHelp();
}

void setup() {
  pinsSetup();
  Serial.begin(115200);
  delay(200);
  banner();

  // 复位脉冲：让寄存器回到手册的复位默认值，便于逐项比对
  digitalWrite(PIN_RST, LOW);
  delay(RST_LOW_MS);
  digitalWrite(PIN_RST, HIGH);
  delay(5);
  pc("#reset pulse done (RST low %dms). BUSY now = %u\n", RST_LOW_MS,
     (unsigned)digitalRead(PIN_BUSY));

  report();
  g_lastReport = millis();
}

void loop() {
  pollSerial();
  const uint32_t now = millis();
  if (g_auto && (uint32_t)(now - g_lastReport) >= REPORT_MS) {
    g_lastReport = now;
    report();
  }
  delay(2);
}
