// ============================================================================
// rf-check —— LoRa 模组体检固件（SX1268 / Ra-01S，**裸 SPI，不用 RadioLib**）
//
// 目的：区分「模组没焊接好」与「模组本身坏了」。
//   数字侧（焊接）：[1] 复位+BUSY  [2] 版本串  [3] 寄存器写回环  [4] 状态字节
//   芯片侧（损坏）：[5] GetDeviceErrors（XOSC 启动 / PLL 锁定 / PA 斜坡 / 校准）
//   射频侧（辐射）：[6] RSSI 噪声底  [7] 发一帧  [8] 连发（配 lora-rx）
//                   [9] 连收（配 lora-tx）
//
// 关键实现约定（昨晚逐条实测得来，见 docs/lora-gateway-test.md §4.7）：
//   · SPI：Mode0 / MSB first / 2MHz；NSS 低开始、高结束
//   · **命令字节之后第一个字节是状态字节**，数据从再下一个开始
//     → 读 N 字节要发 N+1 个 NOP（rx[3]=状态、rx[4]=数据0）
//   · 三条必发命令：SetDio2AsRfSwitchCtrl(0x9D,1)、SetDioIrqParams(0x08)、
//     SetPaConfig(0x95)+SetTxParams(0x8E) —— 缺任一条都会"看起来全对但就是不通"
//
// ⚠️ 串口输出**全 ASCII**（避免终端编码差异导致乱码）；判读表见 README.md（中文）。
// ============================================================================
#include <Arduino.h>
#include <SPI.h>
#include "config.h"

// ---- 命令码（每条都对到 DS_SX1262.pdf 的表号，与 lora-tx/src/config.h 一致）----
#define CMD_CLEAR_IRQ          0x02
#define CMD_CLEAR_DEV_ERRORS   0x07
#define CMD_SET_DIO_IRQ_PARAMS 0x08   // 表 13-28 ★必发（默认全屏蔽）
#define CMD_WRITE_BUFFER       0x0E
#define CMD_WRITE_REGISTER     0x0D
#define CMD_GET_STATS          0x10
#define CMD_GET_PACKET_TYPE    0x11
#define CMD_GET_IRQ_STATUS     0x12
#define CMD_GET_RX_BUFFER_STAT 0x13
#define CMD_GET_PACKET_STATUS  0x14
#define CMD_GET_RSSI_INST      0x15
#define CMD_GET_DEVICE_ERRORS  0x17
#define CMD_READ_REGISTER      0x1D
#define CMD_READ_BUFFER        0x1E
#define CMD_SET_STANDBY        0x80
#define CMD_SET_RX             0x82
#define CMD_SET_TX             0x83
#define CMD_SET_RF_FREQUENCY   0x86
#define CMD_SET_PACKET_TYPE    0x8A
#define CMD_SET_MODULATION     0x8B
#define CMD_SET_PACKET_PARAMS  0x8C
#define CMD_SET_TX_PARAMS      0x8E
#define CMD_SET_BUFFER_BASE    0x8F
#define CMD_SET_PA_CONFIG      0x95   // 表 13-20/21 ★不配 = 默认低功率 PA
#define CMD_SET_DIO2_RF_SWITCH 0x9D   // 表 13-32/33 ★漏发：TX_DONE 正常但空口没辐射
#define CMD_GET_STATUS         0xC0

#define REG_VERSION_STRING     0x0320
#define REG_LORA_SYNC_MSB      0x0740

#define IRQ_TX_DONE    0x0001
#define IRQ_RX_DONE    0x0002
#define IRQ_HEADER_ERR 0x0020
#define IRQ_CRC_ERR    0x0040
#define IRQ_TIMEOUT    0x0200

static float g_freq = RF_FREQ_MHZ;

// ============================== 低层 SPI ==============================
static void led(bool on) { digitalWrite(PIN_LED, on ? LED_ON_LEVEL : !LED_ON_LEVEL); }

static void waitBusy() {
  const uint32_t t0 = millis();
  while (digitalRead(PIN_BUSY) == HIGH && millis() - t0 < BUSY_TIMEOUT_MS) {}
}

// 发命令（可选参数），返回**状态字节**（命令字节之后第 1 个字节）
static uint8_t cmd(uint8_t op, const uint8_t *p, uint8_t n) {
  waitBusy();
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(op);
  for (uint8_t i = 0; i < n; i++) SPI.transfer(p ? p[i] : 0x00);
  const uint8_t st = SPI.transfer(0x00);      // 状态字节
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  return st;
}

// 发命令 + 读 outLen 字节（数据从状态字节之后开始）
static uint8_t cmdRead(uint8_t op, const uint8_t *p, uint8_t n, uint8_t *out,
                       uint8_t outLen) {
  waitBusy();
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(op);
  for (uint8_t i = 0; i < n; i++) SPI.transfer(p ? p[i] : 0x00);
  const uint8_t st = SPI.transfer(0x00);      // 状态字节
  for (uint8_t i = 0; i < outLen; i++) out[i] = SPI.transfer(0x00);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  return st;
}

static uint8_t rdReg(uint16_t addr, uint8_t *out, uint8_t len) {
  const uint8_t a[2] = {(uint8_t)(addr >> 8), (uint8_t)addr};
  return cmdRead(CMD_READ_REGISTER, a, 2, out, len);
}
static uint8_t wrReg(uint16_t addr, const uint8_t *v, uint8_t len) {
  uint8_t buf[12];
  buf[0] = (uint8_t)(addr >> 8);
  buf[1] = (uint8_t)addr;
  for (uint8_t i = 0; i < len; i++) buf[2 + i] = v[i];
  return cmd(CMD_WRITE_REGISTER, buf, 2 + len);
}
static uint16_t getIrq() {
  uint8_t b[2] = {0, 0};
  cmdRead(CMD_GET_IRQ_STATUS, nullptr, 0, b, 2);
  return (uint16_t)((b[0] << 8) | b[1]);
}
static uint16_t getErrors() {
  uint8_t b[2] = {0, 0};
  cmdRead(CMD_GET_DEVICE_ERRORS, nullptr, 0, b, 2);
  return (uint16_t)((b[0] << 8) | b[1]);
}
static void clrIrq() {
  const uint8_t b[2] = {0xFF, 0xFF};
  cmd(CMD_CLEAR_IRQ, b, 2);
}
static void standby() {
  const uint8_t b[1] = {0x00};          // STDBY_RC
  cmd(CMD_SET_STANDBY, b, 1);
}

// ============================== 射频配置 ==============================
static void radioConfig() {
  uint8_t p[8];
  standby();
  p[0] = 0x01; cmd(CMD_SET_PACKET_TYPE, p, 1);              // LoRa
  const uint32_t frf = (uint32_t)((double)g_freq * (double)(1UL << 25) / 32.0);
  p[0] = (uint8_t)(frf >> 24); p[1] = (uint8_t)(frf >> 16);
  p[2] = (uint8_t)(frf >> 8);  p[3] = (uint8_t)frf;
  cmd(CMD_SET_RF_FREQUENCY, p, 4);
  p[0] = 0x04; p[1] = 0x07; p[2] = 0x00; p[3] = 0x01;       // ★PA 配置
  cmd(CMD_SET_PA_CONFIG, p, 4);
  p[0] = TX_POWER_DBM; p[1] = TX_RAMP;                      // ★功率
  cmd(CMD_SET_TX_PARAMS, p, 2);
  p[0] = 0x01; cmd(CMD_SET_DIO2_RF_SWITCH, p, 1);           // ★DIO2 驱动 RF 开关
  p[0] = LORA_SF; p[1] = LORA_BW; p[2] = LORA_CR; p[3] = LORA_LDRO;
  cmd(CMD_SET_MODULATION, p, 4);
  p[0] = 0x00; p[1] = LORA_PREAMBLE; p[2] = LORA_HEADER;
  p[3] = 8;    p[4] = LORA_CRC;      p[5] = LORA_IQ;
  cmd(CMD_SET_PACKET_PARAMS, p, 6);
  // DIO1 = TX_DONE | TIMEOUT（体检项 [7] 要看它有没有翻转）
  const uint8_t irq[8] = {0x02, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00};
  cmd(CMD_SET_DIO_IRQ_PARAMS, irq, 8);                      // ★必发
  p[0] = 0x00; p[1] = 0x00; cmd(CMD_SET_BUFFER_BASE, p, 2);
}

// ============================== 各项体检 ==============================
// [1] 复位 + BUSY 行为
static void t1_reset() {
  Serial.printf("[1] RST/BUSY: idle BUSY=%u", (unsigned)digitalRead(PIN_BUSY));
  digitalWrite(PIN_RST, LOW);
  delay(2);
  const int busyLow = digitalRead(PIN_BUSY);
  digitalWrite(PIN_RST, HIGH);
  delay(10);
  const int busyHigh = digitalRead(PIN_BUSY);
  Serial.printf(" | held-low BUSY=%d | released BUSY=%d", busyLow, busyHigh);
  Serial.printf("  -> %s\n",
                (busyHigh == LOW) ? "PASS"
                                  : "FAIL (BUSY stuck high: wiring or no power)");
}

// [2] 芯片身份（版本串）
static void t2_version() {
  uint8_t v[16] = {0};
  rdReg(REG_VERSION_STRING, v, 16);
  char asc[17];
  for (uint8_t i = 0; i < 16; i++)
    asc[i] = (v[i] >= 0x20 && v[i] <= 0x7E) ? (char)v[i] : '.';
  asc[16] = '\0';
  const bool ok = (memcmp(v, "SX126", 5) == 0);
  Serial.printf("[2] VER 0x0320 = \"%s\"  -> %s\n", asc,
                ok ? "PASS" : "FAIL (SPI/solder: MISO cannot read the module)");
}

// [3] 寄存器写回环（验证 MOSI 与 MISO 都真的通）
static void t3_writeloop() {
  uint8_t saved[2] = {0, 0}, back[2] = {0, 0};
  rdReg(REG_LORA_SYNC_MSB, saved, 2);
  const uint8_t probe[2] = {0x34, 0x44};
  wrReg(REG_LORA_SYNC_MSB, probe, 2);
  rdReg(REG_LORA_SYNC_MSB, back, 2);
  wrReg(REG_LORA_SYNC_MSB, saved, 2);      // 还原 0x1424
  const bool ok = (back[0] == 0x34 && back[1] == 0x44);
  Serial.printf("[3] WR/RD 0x0740: wrote 34 44, read back %02X %02X (orig %02X %02X)"
                "  -> %s\n",
                back[0], back[1], saved[0], saved[1],
                ok ? "PASS" : "FAIL (MOSI or MISO not connected)");
}

// [4] 状态字节稳定性 + chipMode
static void t4_status() {
  Serial.print("[4] STATUS x8:");
  uint8_t last = 0;
  bool stable = true;
  for (int i = 0; i < 8; i++) {
    const uint8_t st = cmd(CMD_GET_STATUS, nullptr, 0);
    Serial.printf(" %02X", st);
    if (i && st != last) stable = false;
    last = st;
  }
  const uint8_t mode = (uint8_t)((last >> 1) & 0x07);   // 2=STBY_RC 5=RX 6=TX
  Serial.printf(" | chipMode=%u (2=STBY_RC)  -> %s\n", (unsigned)mode,
                (stable && mode == 2) ? "PASS" : "CHECK (unstable / odd mode)");
}

// [5] 芯片错误标志（模组自身损坏的主要判据）
static const char *errName(uint16_t e) {
  switch (e) {
    case 0x0001: return "RC64K_CALIB";
    case 0x0002: return "RC13M_CALIB";
    case 0x0004: return "PLL_CALIB";
    case 0x0008: return "ADC_CALIB";
    case 0x0010: return "IMG_CALIB";
    case 0x0020: return "XOSC_START";
    case 0x0040: return "PLL_LOCK";
    case 0x0080: return "PA_RAMP";
    default: return "?";
  }
}
static void t5_errors(const char *tag) {
  const uint16_t e = getErrors();
  Serial.printf("[5] DEV ERR(%s) = 0x%04X", tag, e);
  if (e == 0) {
    Serial.println("  -> PASS");
    return;
  }
  Serial.print("  -> FAIL:");
  for (uint8_t bit = 0; bit < 8; bit++) {          // 错误位只在 bit0..7
    const uint16_t b = (uint16_t)(1u << bit);
    if (e & b) Serial.printf(" %s", errName(b));
  }
  Serial.println("  (XOSC/PLL/PA = module or its XTAL / RF part is bad)");
}

// [6] RSSI 噪声底（接收链路 + 天线）
static void t6_rssi() {
  standby();
  const uint8_t rx[3] = {0xFF, 0xFF, 0xFF};
  cmd(CMD_SET_RX, rx, 3);
  delay(5);
  int16_t mn = 0, mx = -200, sum = 0;
  const int N = 20;
  for (int i = 0; i < N; i++) {
    uint8_t b[1] = {0};
    cmdRead(CMD_GET_RSSI_INST, nullptr, 0, b, 1);
    const int16_t r = -(int16_t)b[0] / 2;
    if (i == 0 || r < mn) mn = r;
    if (r > mx) mx = r;
    sum += r;
    delay(2);
  }
  standby();
  Serial.printf("[6] RSSI(%.1fMHz) min=%d max=%d avg=%d dBm  -> %s\n",
                (double)g_freq, mn, mx, sum / N,
                (mx - mn <= 2) ? "CHECK (flat readings = RX path/antenna suspect)"
                               : "PASS (readings vary = RX chain alive)");
}

// [7] 发一帧（看 TX_DONE / 空口时间 / DIO1 / 之后的误差位）
static void t7_txOnce() {
  radioConfig();
  uint8_t payload[8];
  for (int i = 0; i < 8; i++) payload[i] = (uint8_t)('A' + i);
  const uint8_t base[2] = {0x00, 0x00};
  cmd(CMD_SET_BUFFER_BASE, base, 2);
  uint8_t wb[9];
  wb[0] = 0x00;
  for (int i = 0; i < 8; i++) wb[1 + i] = payload[i];
  cmd(CMD_WRITE_BUFFER, wb, 9);
  clrIrq();
  const uint8_t tx[3] = {0x00, 0x00, 0x00};
  const uint32_t t0 = micros();
  cmd(CMD_SET_TX, tx, 3);
  led(true);
  int dio = 0;
  for (;;) {
    const uint16_t irq = getIrq();
    if (digitalRead(PIN_DIO1) == HIGH) dio = 1;
    if (irq & (IRQ_TX_DONE | IRQ_TIMEOUT)) break;
    if (micros() - t0 > 200000UL) break;      // 200ms 兜底
  }
  const uint32_t dt = micros() - t0;
  const uint16_t irq = getIrq();
  const uint16_t e = getErrors();
  led(false);
  Serial.printf("[7] TX: irq=0x%04X air=%luus (expect ~9280us) dio1=%d err=0x%04X -> %s\n",
                irq, (unsigned long)dt, dio, e,
                (irq & IRQ_TX_DONE) ? "PASS (chip reported TX done)"
                                    : "FAIL (no TX_DONE)");
  Serial.println("        NOTE: TX_DONE only means the chip finished TX, it does NOT"
                 " prove radiation. Use [8] with a lora-rx peer for that.");
  standby();
}

// [8] 连发 10 帧（配 lora-rx 对端判读"到底辐射出去没有"）
static void t8_txBurst() {
  radioConfig();
  Serial.println("[8] sending 10 frames (peer should run lora-rx on the same freq)...");
  for (int k = 0; k < 10; k++) {
    uint8_t pl[8] = {'R', 'F', 'C', 'H', 'K', (uint8_t)('0' + k / 10),
                     (uint8_t)('0' + k % 10), 0};
    const uint8_t base[2] = {0x00, 0x00};
    cmd(CMD_SET_BUFFER_BASE, base, 2);
    uint8_t wb[9];
    wb[0] = 0x00;
    for (int i = 0; i < 8; i++) wb[1 + i] = pl[i];
    cmd(CMD_WRITE_BUFFER, wb, 9);
    clrIrq();
    const uint8_t tx[3] = {0x00, 0x00, 0x00};
    const uint32_t t0 = micros();
    cmd(CMD_SET_TX, tx, 3);
    led(true);
    while (!(getIrq() & (IRQ_TX_DONE | IRQ_TIMEOUT)) && micros() - t0 < 100000UL) {}
    led(false);
    Serial.printf("    #%d sent: irq=0x%04X air=%luus err=0x%04X\n", k, getIrq(),
                  (unsigned long)(micros() - t0), getErrors());
    delay(200);
  }
  standby();
  Serial.println("    -> peer should print 10 packets. If it prints none while [2]-[5]"
                 " PASS: TX path / antenna / PA problem");
}

// [9] 连收 10 秒（配 lora-tx 对端）
static void t9_rx() {
  radioConfig();
  clrIrq();
  const uint8_t rx[3] = {0xFF, 0xFF, 0xFF};
  cmd(CMD_SET_RX, rx, 3);
  Serial.println("[9] receiving 10s (peer should run lora-tx)...");
  uint32_t n = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < 10000) {
    const uint16_t irq = getIrq();
    if (irq & (IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR | IRQ_TIMEOUT)) {
      uint8_t st[2] = {0, 0};
      cmdRead(CMD_GET_RX_BUFFER_STAT, nullptr, 0, st, 2);
      const uint8_t len = st[0] > 8 ? 8 : st[0];
      uint8_t buf[8] = {0};
      if (len) {
        const uint8_t off[1] = {st[1]};
        cmdRead(CMD_READ_BUFFER, off, 1, buf, len);
      }
      uint8_t ps[3] = {0, 0, 0};
      cmdRead(CMD_GET_PACKET_STATUS, nullptr, 0, ps, 3);
      Serial.printf("    irq=0x%04X len=%u rssi=%.1fdBm snr=%.2fdB hex=",
                    irq, (unsigned)len, -(double)ps[0] / 2.0, (double)ps[1] / 4.0);
      for (uint8_t i = 0; i < len; i++) Serial.printf("%02X", buf[i]);
      Serial.print(" asc=\"");
      for (uint8_t i = 0; i < len; i++)
        Serial.print((buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.');
      Serial.println("\"");
      if (irq & IRQ_RX_DONE) n++;
      clrIrq();
      cmd(CMD_SET_RX, rx, 3);
    }
    delay(1);
  }
  standby();
  Serial.printf("    -> received %lu packets. 0 = RX path / antenna (or peer idle)\n",
                (unsigned long)n);
}

static void runAll() {
  Serial.printf("\n===== rf-check : %s / freq=%.3fMHz / SF%u BW500k CR4-5 =====\n",
                BOARD_NAME, (double)g_freq, (unsigned)LORA_SF);
  t1_reset();
  t2_version();
  t3_writeloop();
  standby();
  t4_status();
  t5_errors("after-reset");
  t6_rssi();
  t7_txOnce();
  t5_errors("after-tx");
  Serial.println("    [8]/[9] need a second board: t = send 10 (peer lora-rx),"
                 " r = receive 10s (peer lora-tx)");
  Serial.println("    keys: s=run all | t=tx burst | r=rx 10s | w=write loop |"
                 " e=clear errors | f<MHz>=set freq | i=status | h=help\n");
}

static void help() {
  Serial.println("s=run all  t=tx burst  r=rx 10s  w=write loop  e=clear errors");
  Serial.println("f<MHz>=set freq (e.g. f470.5)  i=status  h=help");
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t = millis(); !Serial && millis() - t < 2500;) delay(20);  // USB-CDC 等主机
  pinMode(PIN_LED, OUTPUT);
  led(false);
  pinMode(PIN_NSS, OUTPUT);
  digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, HIGH);   // 空闲必须高（低有效复位）
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_DIO1, INPUT);
  pinMode(PIN_MISO, INPUT);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  runAll();
}

void loop() {
  static String line;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) {
        const char k = line[0];
        if (k == 's') {
          runAll();
        } else if (k == 't') {
          t8_txBurst();
        } else if (k == 'r') {
          t9_rx();
        } else if (k == 'w') {
          t3_writeloop();
        } else if (k == 'e') {
          cmd(CMD_CLEAR_DEV_ERRORS, nullptr, 0);
          Serial.printf("device errors cleared, now = 0x%04X\n", getErrors());
        } else if (k == 'f') {
          const float f = line.substring(1).toFloat();
          if (f > 400.0f && f < 520.0f) {
            g_freq = f;
            standby();
            Serial.printf("freq set to %.3fMHz (set the peer too)\n", (double)g_freq);
          } else {
            Serial.println("freq range 400~520MHz, e.g. f470.5");
          }
        } else if (k == 'i') {
          Serial.printf("freq=%.3fMHz busy=%u rst=%u dio1=%u err=0x%04X irq=0x%04X\n",
                        (double)g_freq, (unsigned)digitalRead(PIN_BUSY),
                        (unsigned)digitalRead(PIN_RST),
                        (unsigned)digitalRead(PIN_DIO1), getErrors(), getIrq());
        } else {
          help();
        }
        line = "";
      }
    } else if (line.length() < 32) {
      line += c;
    }
  }
}
