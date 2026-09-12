#include <Arduino.h>
#include <SPI.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

// ============================================================================
// lora-tx —— 只发不收：SX1268 LoRa 470MHz 发送端
//
// 上电：复位 → 配置（含 ★0x9D 01） → 每秒自动发一包 "LRTESTnn"
// 对端烧 lora-rx（只收），两边射频参数完全一致。
//
// 命令字节全部按 DS_SX1262.pdf §13（括号内为手册表号）：
//   SetStandby            0x80 + 1        表 13-5
//   SetPacketType         0x8A + 1        表 13-37/38（0x01=LoRa）
//   ★ SetDio2AsRfSwitchCtrl 0x9D + 1     表 13-32/13-33（Enable=1：RX 时 DIO2=0、TX 时 DIO2=1）
//     —— Ra-01S 的收发切换靠 TXEN(脚5)/RXEN(脚11) 的 RF 开关，开关由 DIO2 驱动；
//        板子上 U2 只连了 6 根线（没有 TXEN/RXEN），所以这条**必须发**，
//        否则开关不动：发射时 PA 接不到天线，空口上什么都辐射不出去
//        （而 TX_DONE 仍会正常返回 —— 看起来全对却收不到，就是这个坑）。
//        RadioLib 的 SX126x::begin() 里也固定发这条（SX126x.cpp:192）。
//   ★ SetDioIrqParams     0x08 + 8        表 13-28
//     —— 手册 §13.3.2：**默认所有 IRQ 都被屏蔽（全 0）**，不解除屏蔽时 GetIrqStatus
//        恒为 0x0000（TxDone/RxDone 都不会置位）。本固件轮询 IRQ 寄存器，全靠这条。
//   ★ SetPaConfig          0x95 + 4        表 13-20/13-21
//     —— 不配 = 默认低功率 PA（+14dBm 档）。SX1262/1268 高功率档：
//        paDutyCycle=0x04 hpMax=0x07 deviceSel=0x00 paLut=0x01（表 13-21 的 +22dBm 行）
//   ★ SetTxParams          0x8E + 2        表 13-40：功率(dBm) + 上升时间(表 13-41)
//   SetRfFrequency        0x86 + 4        表 13-36（RfFreq = f*2^25/32e6）
//   SetModulationParams   0x8B + 4        表 13-47/48/49/50（SF/BW/CR/LDRO）
//   SetPacketParams       0x8C + 6        表 13-66..70（前导/包头/长度/CRC/IQ）
//   SetBufferBaseAddress  0x8F + 2        表 13-75
//   WriteBuffer           0x0E + off + N  表 13-26
//   SetTx                 0x83 + 3        表 13-6/7（timeout 单位 15.625µs，0=不超时）
//   GetIrqStatus          0x12 + NOP×2    表 13-30 → IrqStatus 在 rx[2..3]
//   GetPacketType         0x11 → rx[2]    表 13-39
//   GetStats              0x10 → rx[2..7] 表 13-83
//   GetDeviceErrors       0x17 → rx[2..3] 表 13-84
//   ReadRegister          0x1D + 2 地址 + NOP(Status) + N×NOP → 数据从 rx[4] 起  表 13-25
//
//   ⚠️ 共同规律：返回数据的命令，命令/地址之后先有一个 **Status 字节**，数据在它后面。
//
// 串口命令（行末回车）：
//   t 立刻发一包        T 自动发送开关（默认 ON，每 TX_PERIOD_MS 一包）
//   p 周期切换 200/500/1000/2000 ms      a 立刻打印状态行
//   r 重新配置          s SPI 时钟换档   <地址>[.<n>] 读寄存器   h 帮助
// ============================================================================

#define IRQ_TX_DONE 0x0001
#define IRQ_TIMEOUT 0x0200

static uint32_t g_hz = SPI_HZ_DEFAULT;
static uint16_t g_periodMs = TX_PERIOD_MS;
static bool     g_autoTx = true;
static uint32_t g_busyTo = 0;
static uint16_t g_seq = 0;
static uint32_t g_txOk = 0, g_txErr = 0;
static uint32_t g_toaUs = 0;
static uint32_t g_airMin = 0, g_airMax = 0, g_airSum = 0, g_airN = 0;
static uint32_t g_lastTx = 0, g_lastSum = 0;

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

// ---------------------------------------------------------------- 引脚 / 总线
static void pinsSetup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, (LED_ON_LEVEL == HIGH) ? LOW : HIGH);
  pinMode(PIN_NSS, OUTPUT);
  digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_MISO, INPUT);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
}

static bool busyWait() {
  const uint32_t t0 = micros();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if ((uint32_t)(micros() - t0) > BUSY_WAIT_US) { g_busyTo++; return false; }
  }
  return true;
}

static void xfer(const uint8_t *tx, uint8_t *rx, size_t n) {
  busyWait();
  SPI.beginTransaction(SPISettings(g_hz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transferBytes(tx, rx, (uint32_t)n);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
}

static void cmdWrite(uint8_t cmd, const uint8_t *data, uint8_t n) {
  uint8_t tx[24], rx[24];
  if (n > 23) n = 23;
  tx[0] = cmd;
  for (uint8_t i = 0; i < n; i++) tx[i + 1] = data[i];
  xfer(tx, rx, (size_t)n + 1);
}

// 读命令：opcode + NOP(Status) + N×NOP -> 数据从 rx[2] 起
static void cmdReadPayload(uint8_t cmd, uint8_t *out, uint8_t n) {
  uint8_t tx[12], rx[12];
  if (n > 9) n = 9;
  memset(tx, 0, sizeof(tx));
  memset(rx, 0, sizeof(rx));
  tx[0] = cmd;
  xfer(tx, rx, (size_t)n + 2);
  memcpy(out, rx + 2, n);
}

static uint8_t cmdStatus() {                 // 0xC0 + NOP -> rx[1] 就是 Status
  uint8_t tx[2] = {CMD_GET_STATUS, 0x00}, rx[2] = {0, 0};
  xfer(tx, rx, 2);
  return rx[1];
}

static uint8_t cmdPacketType() {
  uint8_t d[1] = {0};
  cmdReadPayload(CMD_GET_PACKET_TYPE, d, 1);
  return d[0];
}

static uint16_t cmdDeviceErrors() {
  uint8_t d[2] = {0, 0};
  cmdReadPayload(CMD_GET_DEVICE_ERRORS, d, 2);
  return (uint16_t)(((uint16_t)d[0] << 8) | d[1]);
}

static uint16_t cmdGetIrq() {
  uint8_t d[2] = {0, 0};
  cmdReadPayload(CMD_GET_IRQ_STATUS, d, 2);
  return (uint16_t)(((uint16_t)d[0] << 8) | d[1]);
}

static void cmdClearIrq(uint16_t mask) {
  uint8_t d[2] = {(uint8_t)(mask >> 8), (uint8_t)mask};
  cmdWrite(CMD_CLEAR_IRQ, d, 2);
}

static void cmdSetBufferBase(uint8_t txBase, uint8_t rxBase) {
  uint8_t d[2] = {txBase, rxBase};
  cmdWrite(CMD_SET_BUFFER_BASE, d, 2);
}

static void cmdWriteBuffer(uint8_t off, const uint8_t *data, uint8_t n) {
  uint8_t tx[24], rx[24];
  if (n > 22) n = 22;
  tx[0] = CMD_WRITE_BUFFER;
  tx[1] = off;
  for (uint8_t i = 0; i < n; i++) tx[i + 2] = data[i];
  xfer(tx, rx, (size_t)n + 2);
}

static void cmdSetTx(uint32_t timeout) {
  uint8_t d[3] = {(uint8_t)(timeout >> 16), (uint8_t)(timeout >> 8), (uint8_t)timeout};
  cmdWrite(CMD_SET_TX, d, 3);
}

static void cmdSetStandby() {
  uint8_t d[1] = {0x00};                     // STDBY_RC
  cmdWrite(CMD_SET_STANDBY, d, 1);
}

static void cmdReadRegs(uint16_t addr, uint8_t *out, uint8_t n) {
  uint8_t tx[20], rx[20];
  if (n > 16) n = 16;
  memset(tx, 0, sizeof(tx));
  memset(rx, 0, sizeof(rx));
  tx[0] = CMD_READ_REGISTER;
  tx[1] = (uint8_t)(addr >> 8);
  tx[2] = (uint8_t)(addr & 0xFF);
  xfer(tx, rx, (size_t)n + 4);
  memcpy(out, rx + 4, n);
}

// ---------------------------------------------------------------- 配置
static uint32_t rfFreqWord() {
  return (uint32_t)((double)RADIO_FREQ_MHZ * 33554432.0 / 32.0);
}

static void setPacketParams(uint8_t payloadLen) {
  uint8_t d[6];
  d[0] = (uint8_t)(LORA_PREAMBLE >> 8);
  d[1] = (uint8_t)LORA_PREAMBLE;
  d[2] = LORA_HEADER;
  d[3] = payloadLen;
  d[4] = LORA_CRC;
  d[5] = LORA_IQ;
  cmdWrite(CMD_SET_PACKET_PARAMS, d, 6);
}

static void configure(bool verbose) {
  uint8_t one = 0x01;
  const uint32_t frf = rfFreqWord();
  uint8_t df[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8),
                   (uint8_t)frf};
  uint8_t dm[4] = {LORA_SF, LORA_BW, LORA_CR, LORA_LDRO};
  // ★ SetDioIrqParams：IrqMask=0x03FF（表 13-29 的全部 10 个 IRQ 位），DIO1/2/3 不映射
  //   手册 §13.3.2：默认所有 IRQ 被屏蔽（全 0），不解除屏蔽 → GetIrqStatus 恒为 0x0000
  uint8_t irq[8] = {0x03, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  // 表 13-21：SX1262/1268 高功率档 paDutyCycle=0x04 hpMax=0x07 deviceSel=0x00 paLut=0x01
  uint8_t pa[4] = {0x04, 0x07, 0x00, 0x01};
  uint8_t tp[2] = {TX_POWER_DBM, TX_RAMP};

  cmdSetStandby();
  cmdWrite(CMD_SET_PACKET_TYPE, &one, 1);            // LoRa
  cmdWrite(CMD_SET_DIO2_RF_SWITCH, &one, 1);         // ★0x9D 01：DIO2 驱动 RF 开关
  cmdWrite(CMD_SET_DIO_IRQ_PARAMS, irq, 8);          // ★0x08：解除 IRQ 屏蔽
  cmdWrite(CMD_SET_PA_CONFIG, pa, 4);                // ★0x95：高功率 PA
  cmdWrite(CMD_SET_TX_PARAMS, tp, 2);                // ★0x8E：+15dBm / 200µs ramp
  cmdWrite(CMD_SET_RF_FREQUENCY, df, 4);
  cmdWrite(CMD_SET_MODULATION, dm, 4);
  setPacketParams(LORA_PAYLOAD_LEN);
  cmdClearIrq(0xFFFF);

  if (verbose) {
    pc("#cfg standby / packetType=LoRa / DIO2-as-RF-switch=1 (0x9D 01) / IrqMask=0x03FF"
       " (0x08) / PaConfig=04 07 00 01 (0x95) / TxParams=pwr %d ramp 200us (0x8E)\n",
       (int)TX_POWER_DBM);
    pc("#cfg rfFreq=0x%08lX (%.3fMHz) / SF%u BW500k CR4-5 / preamble %u explicit CRC on"
       " / len %u\n",
       (unsigned long)frf, (double)RADIO_FREQ_MHZ, (unsigned)LORA_SF,
       (unsigned)LORA_PREAMBLE, (unsigned)LORA_PAYLOAD_LEN);
  }
}

// ---------------------------------------------------------------- 理论空口时间
static uint32_t expectedToA_us(uint8_t pl) {
  const uint32_t tsym = (uint32_t)(((uint32_t)1 << LORA_SF) * 1000000UL / 500000UL);
  const uint32_t pre = (uint32_t)((LORA_PREAMBLE + 4.25) * (double)tsym);
  const int32_t de = LORA_LDRO ? 1 : 0;
  const int32_t num = 8 * (int32_t)pl - 4 * (int32_t)LORA_SF + 28 +
                      16 * (int32_t)LORA_CRC - 20 * (int32_t)(LORA_HEADER ? 1 : 0);
  const int32_t den = 4 * ((int32_t)LORA_SF - 2 * de);
  int32_t n = (num + den - 1) / den;
  if (n < 0) n = 0;
  const uint32_t pay = (uint32_t)(8 + n * (4 + (int32_t)LORA_CR)) * tsym;
  return pre + pay;
}

// ---------------------------------------------------------------- 发一包
static void sendPacket() {
  uint8_t payload[LORA_PAYLOAD_LEN];
  char tmp[16], txt[LORA_PAYLOAD_LEN + 1];
  g_seq++;
  snprintf(tmp, sizeof(tmp), "LRTEST%02u", (unsigned)(g_seq % 100));
  memcpy(payload, tmp, LORA_PAYLOAD_LEN);
  memcpy(txt, payload, LORA_PAYLOAD_LEN);
  txt[LORA_PAYLOAD_LEN] = 0;

  cmdSetStandby();
  setPacketParams(LORA_PAYLOAD_LEN);
  cmdSetBufferBase(0x00, 0x00);
  cmdWriteBuffer(0x00, payload, LORA_PAYLOAD_LEN);
  cmdClearIrq(0xFFFF);

  const uint32_t t0 = micros();
  cmdSetTx(0x000000UL);                       // 不启用超时时钟（发完自动回 STDBY_RC）
  uint16_t irq = 0;
  while ((uint32_t)(micros() - t0) < 40000UL) {   // ToA≈8ms，等 40ms 足够
    irq = cmdGetIrq();
    if (irq & (IRQ_TX_DONE | IRQ_TIMEOUT)) break;
  }
  const uint32_t dt = micros() - t0;
  cmdClearIrq(0xFFFF);

  bool ok = (irq & IRQ_TX_DONE) != 0;
  // 兜底：IRQ 没置起来时，看状态字节的命令状态字段（6 = Command TX done，表 13-77）
  uint8_t stByte = 0;
  bool stDone = false;
  if (!ok) {
    stByte = cmdStatus();
    stDone = (((stByte >> 1) & 0x07) == 0x06);
  }
  if (ok || stDone) {
    g_txOk++;
    if (g_airN == 0) { g_airMin = g_airMax = dt; }
    else { if (dt < g_airMin) g_airMin = dt; if (dt > g_airMax) g_airMax = dt; }
    g_airSum += dt;
    if (g_airN < 0xFFFF) g_airN++;
  } else {
    g_txErr++;
  }

  if (ok) {
    pc("#tx  seq=%u data=\"%s\"  irq=0x%04X TX_DONE  air=%.2fms (expected %.2fms)\n",
       (unsigned)g_seq, txt, (unsigned)irq, (double)dt / 1000.0,
       (double)g_toaUs / 1000.0);
  } else if (stDone) {
    pc("#tx  seq=%u data=\"%s\"  irq=0x0000 no-irq but status=0x%02X (cmd=TX done)"
       " -> sent; IRQ flag missing, check SetDioIrqParams(0x08)\n",
       (unsigned)g_seq, txt, (unsigned)stByte);
  } else {
    pc("#tx  seq=%u data=\"%s\"  irq=0x%04X status=0x%02X FAILED  air=%.2fms"
       "   <- 没发出去：查 standby / 0x9D / 0x95+0x8E(PA) / 供电 / 天线\n",
       (unsigned)g_seq, txt, (unsigned)irq, (unsigned)stByte, (double)dt / 1000.0);
  }

  digitalWrite(PIN_LED, LED_ON_LEVEL);
  delay(3);
  digitalWrite(PIN_LED, (LED_ON_LEVEL == HIGH) ? LOW : HIGH);
}

// ---------------------------------------------------------------- 状态 / 汇总
static void report() {
  uint8_t ver[17];
  memset(ver, 0, sizeof(ver));
  cmdReadRegs(REG_VERSION_STRING, ver, 16);
  const uint8_t st = cmdStatus();
  const uint8_t pkt = cmdPacketType();
  const uint16_t err = cmdDeviceErrors();
  uint8_t sync[2] = {0, 0};
  cmdReadRegs(REG_LORA_SYNC_MSB, sync, 2);
  const uint32_t airAvg = g_airN ? (g_airSum / g_airN) : 0;

  pc("#st  st=0x%02X(%s)  pkt=0x%02X(%s)  err=0x%04X  tx: ok=%lu err=%lu"
     "  air min/avg/max=%.2f/%.2f/%.2f ms  period=%ums  busyTo=%lu\n",
     (unsigned)st,
     ((st & 0x70) == 0x20) ? "STBY_RC" : (((st & 0x70) == 0x60) ? "TX" : "?"),
     (unsigned)pkt, (pkt == 0x01) ? "LoRa" : (pkt == 0x00 ? "FSK" : "?"),
     (unsigned)err, (unsigned long)g_txOk, (unsigned long)g_txErr,
     (double)g_airMin / 1000.0, (double)airAvg / 1000.0, (double)g_airMax / 1000.0,
     (unsigned)g_periodMs, (unsigned long)g_busyTo);
  pc("    ver=%.16s  sync=0x%02X%02X  autoTx=%s\n", (const char *)ver, sync[0],
     sync[1], g_autoTx ? "ON" : "OFF");
}

// ---------------------------------------------------------------- 读任意地址
static void readAddrCmd(const char *s) {
  char buf[24];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  uint8_t len = 1;
  char *dot = strchr(buf, '.');
  if (dot) {
    *dot = 0;
    const long l = atol(dot + 1);
    len = (l < 1) ? 1 : (l > 16 ? 16 : (uint8_t)l);
  }
  const long addr = strtol(buf, NULL, 16);
  if (addr < 0 || addr > 0xFFFF) { pc("#? bad address \"%s\"\n", s); return; }
  uint8_t tx[20], rx[20], v[16];
  memset(tx, 0, sizeof(tx));
  memset(rx, 0, sizeof(rx));
  tx[0] = CMD_READ_REGISTER;
  tx[1] = (uint8_t)((uint16_t)addr >> 8);
  tx[2] = (uint8_t)((uint16_t)addr & 0xFF);
  xfer(tx, rx, (size_t)len + 4);
  memcpy(v, rx + 4, len);
  pc("#read 0x%04X x%u =", (unsigned)addr, (unsigned)len);
  for (uint8_t i = 0; i < len; i++) pc(" %02X", v[i]);
  pc("  (");
  for (uint8_t i = 0; i < len; i++) pc("%c", (v[i] >= 0x20 && v[i] <= 0x7E) ? v[i] : '.');
  pc(")   raw: status=%02X data=", rx[3]);
  for (uint8_t i = 0; i < len; i++) pc(" %02X", rx[4 + i]);
  pc("\n");
}

// ---------------------------------------------------------------- 帮助 / 命令
static void printHelp() {
  pc("keys (line ends with Enter):\n");
  pc("  t          send one packet now\n");
  pc("  T          auto-send on/off (default ON, one packet per %ums)\n",
     (unsigned)TX_PERIOD_MS);
  pc("  p          cycle period 200/500/1000/2000 ms\n");
  pc("  a          print one status line\n");
  pc("  r          re-run the config sequence\n");
  pc("  s          SPI clock: 1M -> 2M -> 4M -> 8M -> 16M MHz\n");
  pc("  0740 / 0320.16   read register(s), with raw bytes\n");
  pc("  h          help\n");
}

static void stepClock() {
  static const uint32_t hz[5] = {1000000UL, 2000000UL, 4000000UL, 8000000UL, 16000000UL};
  uint8_t i = 0;
  for (i = 0; i < 5; i++) if (hz[i] == g_hz) break;
  g_hz = hz[(i + 1) % 5];
  pc("#clk = %lukHz\n", (unsigned long)(g_hz / 1000));
}

static void stepPeriod() {
  static const uint16_t ms[4] = {200, 500, 1000, 2000};
  uint8_t i = 0;
  for (i = 0; i < 4; i++) if (ms[i] == g_periodMs) break;
  g_periodMs = ms[(i + 1) % 4];
  pc("#period = %ums\n", (unsigned)g_periodMs);
}

static char s_line[32];
static uint8_t s_len = 0;

static void onLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;
  const char c = line[0];
  if (c == 't') { sendPacket(); return; }
  if (c == 'T') {
    g_autoTx = !g_autoTx;
    pc("#auto-send = %s\n", g_autoTx ? "ON" : "OFF");
    return;
  }
  if (c == 'p') { stepPeriod(); return; }
  if (c == 'a') { report(); return; }
  if (c == 'r') { configure(true); report(); return; }
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
    const char ch = (char)Serial.read();
    if (ch == '\r' || ch == '\n') {
      if (s_len) { s_line[s_len] = 0; s_len = 0; onLine(s_line); }
      continue;
    }
    if (ch == 8 || ch == 127) { if (s_len) s_len--; continue; }
    if (s_len < sizeof(s_line) - 1) s_line[s_len++] = ch;
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  pinsSetup();
  Serial.begin(115200);
  delay(200);

  pc("\n===== lora-tx : SX1268 LoRa TX only (470MHz SF7 BW500k) =====\n");
  pc("board : %s\n", BOARD_NAME);
  pc("spi   : NSS=G%d SCK=G%d MOSI=G%d MISO=G%d  mode0  %lukHz\n", (int)PIN_NSS,
     (int)PIN_SCK, (int)PIN_MOSI, (int)PIN_MISO, (unsigned long)(g_hz / 1000));
  pc("peer  : flash lora-rx on the other board (same RF settings)\n");

  digitalWrite(PIN_RST, LOW);
  delay(RST_LOW_MS);
  digitalWrite(PIN_RST, HIGH);
  delay(5);

  configure(true);
  g_toaUs = expectedToA_us(LORA_PAYLOAD_LEN);
  pc("#toa expected = %.2fms for %u bytes (SF%u/BW500k/CR4-5/preamble %u/CRC on)\n",
     (double)g_toaUs / 1000.0, (unsigned)LORA_PAYLOAD_LEN, (unsigned)LORA_SF,
     (unsigned)LORA_PREAMBLE);
  report();
  pc("#auto-sending one packet every %ums. 'T' to stop, 't' for single shot.\n",
     (unsigned)g_periodMs);
  printHelp();
  g_lastTx = millis();
  g_lastSum = millis();
}

void loop() {
  const uint32_t now = millis();
  pollSerial();

  if (g_autoTx && (uint32_t)(now - g_lastTx) >= g_periodMs) {
    g_lastTx = now;
    sendPacket();
  }
  if ((uint32_t)(now - g_lastSum) >= TX_SUM_MS) {
    g_lastSum = now;
    report();
  }
  delay(2);
}
