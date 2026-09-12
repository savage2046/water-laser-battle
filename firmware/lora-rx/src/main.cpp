#include <Arduino.h>
#include <SPI.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

// ============================================================================
// lora-rx —— 只收不发：SX1268 LoRa 470MHz 接收端
//
// 上电：复位 → 配置（含 ★0x9D 01） → 连续接收 → 每秒打印底噪 RSSI，
//       收到包立刻打印 len / rssi / snr / sigRssi / 数据(hex+ascii)。
// 对端烧 lora-tx（只发），两边射频参数完全一致。
//
// 命令字节全部按 DS_SX1262.pdf §13（括号内为手册表号）：
//   SetStandby            0x80 + 1        表 13-5
//   SetPacketType         0x8A + 1        表 13-37/38（0x01=LoRa）
//   ★ SetDio2AsRfSwitchCtrl 0x9D + 1     表 13-32/13-33（Enable=1：RX 时 DIO2=0、TX 时 DIO2=1）
//     —— Ra-01S 的收发切换靠 TXEN(脚5)/RXEN(脚11) 的 RF 开关，开关由 DIO2 驱动；
//        板子上 U2 只连了 6 根线（没有 TXEN/RXEN），所以这条**必须发**。
//        漏发它的症状：**RSSI 一个字节都不变地卡在某个值**（接收端看到的是被隔离的
//        前端，不是天线与环境），而且发射端会"TX_DONE 正常但空口上没有辐射"。
//        RadioLib 的 SX126x::begin() 里也固定发这条（SX126x.cpp:192）。
//   ★ SetDioIrqParams     0x08 + 8        表 13-28
//     —— 手册 §13.3.2：**默认所有 IRQ 都被屏蔽（全 0）**，不解除屏蔽时 GetIrqStatus
//        恒为 0x0000 —— 那样 RxDone/CrcErr 永远看不到，接收端表现成"什么都收不到"。
//        本固件轮询 IRQ 寄存器，全靠这条。
//   SetRfFrequency        0x86 + 4        表 13-36（RfFreq = f*2^25/32e6）
//   SetModulationParams   0x8B + 4        表 13-47/48/49/50（SF/BW/CR/LDRO）
//   SetPacketParams       0x8C + 6        表 13-66..70（前导/包头/最大长度/CRC/IQ）
//   SetRx                 0x82 + 3        表 13-8/9（0xFFFFFF=连续接收）
//   GetIrqStatus          0x12 + NOP×2    表 13-30 → IrqStatus 在 rx[2..3]
//   ClearIrqStatus        0x02 + 2        表 13-31
//   GetRxBufferStatus     0x13 → rx[2]=长度, rx[3]=起始地址        表 13-79
//   ReadBuffer            0x1E + off + NOP(Status) + N×NOP → 数据从 rx[3] 起  表 13-27
//   GetPacketStatus       0x14 → rx[2]=RssiPkt, rx[3]=SnrPkt, rx[4]=SignalRssiPkt  表 13-80
//   GetRssiInst           0x15 → rx[2]，dBm = -raw/2（raw 无符号）  表 13-82
//   GetStats              0x10 → rx[2..7] 三个 16 位计数            表 13-83
//   GetDeviceErrors       0x17 → rx[2..3]                          表 13-84
//   ReadRegister          0x1D + 2 地址 + NOP(Status) + N×NOP → 数据从 rx[4] 起  表 13-25
//
//   ⚠️ 共同规律：返回数据的命令，命令/地址之后先有一个 **Status 字节**，数据在它后面。
//
// 串口命令（行末回车）：
//   a 立刻打印报告      l 每秒报告开关      r 重新配置并进接收
//   f<MHz> 改频点（如 f470.5）             n 底噪扫描 460~480MHz 每 2MHz 一行
//   s SPI 时钟换档      <地址>[.<n>] 读寄存器     h 帮助
// ============================================================================

// ---------------------------------------------------------------- IRQ 位（表 13-29）
#define IRQ_RX_DONE    0x0002
#define IRQ_HEADER_ERR 0x0020
#define IRQ_CRC_ERR    0x0040
#define IRQ_TIMEOUT    0x0200

static uint32_t g_hz = SPI_HZ_DEFAULT;
static double   g_freqMHz = RADIO_FREQ_MHZ;
static bool     g_auto = true;
static uint32_t g_busyTo = 0;
static uint32_t g_n = 0;
static uint32_t g_lastReport = 0;
static uint16_t g_rxCount = 0, g_crcCount = 0, g_hdrCount = 0, g_toCount = 0;
static uint32_t g_lastPktMs = 0;
static int8_t   g_lastRssi = 0;
static uint8_t  g_lastSnr = 128;      // 128 = 未见包
// 丢包统计：载荷是 "LRTESTnn"（nn 从 00 到 99 循环），按序号连续性算 PER
static int32_t  g_lastSeq = -1;
static uint32_t g_seqRx = 0, g_seqMissing = 0, g_seqDup = 0;
static int8_t   g_lastRssiPkt = 0;
static uint8_t  g_lastSnrPkt = 128;

// ---------------------------------------------------------------- 串口（纯 ASCII）
static void pc(const char *fmt, ...) {
  if (!(bool)Serial) return;
  char b[288];
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

static void cmdReadPayload(uint8_t cmd, uint8_t *out, uint8_t n) {
  uint8_t tx[12], rx[12];
  if (n > 9) n = 9;
  memset(tx, 0, sizeof(tx));
  memset(rx, 0, sizeof(rx));
  tx[0] = cmd;
  xfer(tx, rx, (size_t)n + 2);
  memcpy(out, rx + 2, n);
}

static uint8_t cmdStatus() {
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

static uint8_t cmdRssiInst() {
  uint8_t d[1] = {0};
  cmdReadPayload(CMD_GET_RSSI_INST, d, 1);
  return d[0];
}

static void cmdStats(uint16_t *rx, uint16_t *crc, uint16_t *hdr) {
  uint8_t d[6] = {0, 0, 0, 0, 0, 0};
  cmdReadPayload(CMD_GET_STATS, d, 6);
  *rx  = (uint16_t)(((uint16_t)d[0] << 8) | d[1]);
  *crc = (uint16_t)(((uint16_t)d[2] << 8) | d[3]);
  *hdr = (uint16_t)(((uint16_t)d[4] << 8) | d[5]);
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

static void cmdSetRx(uint32_t timeout) {
  uint8_t d[3] = {(uint8_t)(timeout >> 16), (uint8_t)(timeout >> 8), (uint8_t)timeout};
  cmdWrite(CMD_SET_RX, d, 3);
}

static void cmdSetStandby() {
  uint8_t d[1] = {0x00};                     // STDBY_RC
  cmdWrite(CMD_SET_STANDBY, d, 1);
}

static void cmdGetRxBufferStatus(uint8_t *len, uint8_t *off) {
  uint8_t d[2] = {0, 0};
  cmdReadPayload(CMD_GET_RX_BUFFER_STATUS, d, 2);
  *len = d[0];
  *off = d[1];
}

static void cmdGetPacketStatus(uint8_t *rssiPkt, int8_t *snrDiv4, uint8_t *sigRssi) {
  uint8_t d[3] = {0, 0, 0};
  cmdReadPayload(CMD_GET_PACKET_STATUS, d, 3);
  *rssiPkt = d[0];
  *snrDiv4 = (int8_t)d[1];
  *sigRssi = d[2];
}

// ReadBuffer：opcode + offset + NOP(Status) + N×NOP -> 数据从 rx[3] 起
static void cmdReadBuffer(uint8_t off, uint8_t *out, uint8_t n) {
  uint8_t tx[24], rx[24];
  if (n > 20) n = 20;
  memset(tx, 0, sizeof(tx));
  memset(rx, 0, sizeof(rx));
  tx[0] = CMD_READ_BUFFER;
  tx[1] = off;
  xfer(tx, rx, (size_t)n + 3);
  memcpy(out, rx + 3, n);
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
  return (uint32_t)(g_freqMHz * 33554432.0 / 32.0);
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
  //   → RxDone/CrcErr 永远看不到，接收端会"什么都收不到"
  uint8_t irq[8] = {0x03, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  cmdSetStandby();
  cmdWrite(CMD_SET_PACKET_TYPE, &one, 1);            // LoRa
  cmdWrite(CMD_SET_DIO2_RF_SWITCH, &one, 1);         // ★0x9D 01：DIO2 驱动 RF 开关
  cmdWrite(CMD_SET_DIO_IRQ_PARAMS, irq, 8);          // ★0x08：解除 IRQ 屏蔽
  cmdWrite(CMD_SET_RF_FREQUENCY, df, 4);
  cmdWrite(CMD_SET_MODULATION, dm, 4);
  setPacketParams(LORA_PAYLOAD_MAX);
  cmdClearIrq(0xFFFF);
  cmdSetRx(RX_TIMEOUT_CONT);                         // 连续接收

  if (verbose) {
    pc("#cfg standby / packetType=LoRa / DIO2-as-RF-switch=1 (0x9D 01) / IrqMask=0x03FF"
       " (0x08) / rfFreq=0x%08lX (%.3fMHz) / SF%u BW500k CR4-5 / preamble %u explicit"
       " CRC on / maxLen %u / SetRx=continuous\n",
       (unsigned long)frf, g_freqMHz, (unsigned)LORA_SF, (unsigned)LORA_PREAMBLE,
       (unsigned)LORA_PAYLOAD_MAX);
    pc("#cfg (RX only: no SetPaConfig/SetTxParams needed)\n");
  }
}

// 载荷序号解析 + 丢包统计（载荷格式 "LRTESTnn"，nn = 00..99 循环）
static void updatePer(const uint8_t *buf, uint8_t len) {
  if (len < 8) return;
  if (memcmp(buf, "LRTEST", 6) != 0) return;
  if (buf[6] < '0' || buf[6] > '9' || buf[7] < '0' || buf[7] > '9') return;
  const int32_t seq = (int32_t)(buf[6] - '0') * 10 + (buf[7] - '0');
  if (g_lastSeq < 0) { g_lastSeq = seq; g_seqRx = 1; return; }
  int32_t d = seq - g_lastSeq;
  if (d < 0) d += 100;                    // 序号 00..99 循环
  if (d == 0 || d >= 50) { g_seqDup++; return; }   // 重复或乱序，不计
  g_seqMissing += (uint32_t)(d - 1);
  g_seqRx++;
  g_lastSeq = seq;
}

// ---------------------------------------------------------------- 收包
static void pollRx() {
  const uint16_t irq = cmdGetIrq();
  const uint16_t ev = IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR | IRQ_TIMEOUT;
  if (!(irq & ev)) return;
  cmdClearIrq((uint16_t)(irq & ev));

  if (irq & IRQ_HEADER_ERR) {
    g_hdrCount++;
    pc("#rx  header error (LoRa header CRC bad)  count=%u\n", (unsigned)g_hdrCount);
  }
  if (irq & IRQ_CRC_ERR) {
    g_crcCount++;
    uint8_t rp = 0, sig = 0; int8_t sn = 0;
    cmdGetPacketStatus(&rp, &sn, &sig);
    pc("#rx  CRC error  rssi=%.1f snr=%.2f dB  count=%u   <- something arrived but did not"
       " decode (sync word / params mismatch, interference, or too far)\n",
       -1.0 * (double)rp / 2.0, (double)sn / 4.0, (unsigned)g_crcCount);
  }
  if (irq & IRQ_TIMEOUT) {
    g_toCount++;
    pc("#rx  rx timeout  count=%u\n", (unsigned)g_toCount);
  }

  if (irq & IRQ_RX_DONE) {
    uint8_t len = 0, off = 0;
    cmdGetRxBufferStatus(&len, &off);
    uint8_t buf[32];
    char hex[3 * 32 + 1], asc[33];
    memset(buf, 0, sizeof(buf));
    if (len > sizeof(buf)) len = (uint8_t)sizeof(buf);
    cmdReadBuffer(off, buf, len);
    size_t p = 0;
    for (uint8_t i = 0; i < len; i++)
      p += (size_t)snprintf(hex + p, sizeof(hex) - p, "%02X ", buf[i]);
    if (p) hex[p - 1] = 0;
    for (uint8_t i = 0; i < len; i++)
      asc[i] = (buf[i] >= 0x20 && buf[i] <= 0x7E) ? (char)buf[i] : '.';
    asc[len] = 0;

    uint8_t rp = 0, sig = 0; int8_t sn = 0;
    cmdGetPacketStatus(&rp, &sn, &sig);
    g_rxCount++;
    g_lastRssi = (int8_t)(-1.0 * (double)rp / 2.0);
    g_lastSnr = (uint8_t)sn;
    g_lastPktMs = millis();
    g_lastRssiPkt = g_lastRssi;
    g_lastSnrPkt = g_lastSnr;
    updatePer(buf, len);
    pc("#rx  #%u len=%u  rssi=%.1fdBm  snr=%.2fdB  sigRssi=%.1fdBm  data=\"%s\"  (%s)\n",
       (unsigned)g_rxCount, (unsigned)len, -1.0 * (double)rp / 2.0, (double)sn / 4.0,
       -1.0 * (double)sig / 2.0, asc, hex);

    digitalWrite(PIN_LED, LED_ON_LEVEL);       // 收到闪一下
    delay(5);
    digitalWrite(PIN_LED, (LED_ON_LEVEL == HIGH) ? LOW : HIGH);
  }
}

// ---------------------------------------------------------------- 报告
static void report() {
  g_n++;
  uint8_t ver[17];
  memset(ver, 0, sizeof(ver));
  cmdReadRegs(REG_VERSION_STRING, ver, 16);
  const uint8_t st = cmdStatus();
  const uint8_t pkt = cmdPacketType();
  const uint16_t err = cmdDeviceErrors();
  const uint8_t raw = cmdRssiInst();
  uint16_t nrx = 0, ncrc = 0, nhdr = 0;
  cmdStats(&nrx, &ncrc, &nhdr);
  uint8_t sync[2] = {0, 0};
  cmdReadRegs(REG_LORA_SYNC_MSB, sync, 2);

  pc("#%lu %.3fMHz rssi=%.1fdBm  st=0x%02X(%s)  pkt=0x%02X(%s)  err=0x%04X"
     "  stats: pkts=%u crc=%u hdr=%u | mine: rx=%u crc=%u hdr=%u to=%u | busyTo=%lu\n",
     (unsigned long)g_n, g_freqMHz, -1.0 * (double)raw / 2.0, (unsigned)st,
     ((st & 0x70) == 0x50) ? "RX" : (((st & 0x70) == 0x20) ? "STBY_RC" : "?"),
     (unsigned)pkt, (pkt == 0x01) ? "LoRa" : (pkt == 0x00 ? "FSK" : "?"),
     (unsigned)err, (unsigned)nrx, (unsigned)ncrc, (unsigned)nhdr,
     (unsigned)g_rxCount, (unsigned)g_crcCount, (unsigned)g_hdrCount,
     (unsigned)g_toCount, (unsigned long)g_busyTo);
  pc("   ver=%.16s  sync=0x%02X%02X%s\n", (const char *)ver, sync[0], sync[1],
     (memcmp(ver, "SX126", 5) == 0 || memcmp(ver, "LLCC68", 6) == 0)
         ? ""
         : "   <- version unreadable, check SPI");
  if (g_seqRx > 0) {
    const uint32_t denom = g_seqRx + g_seqMissing;
    pc("   PER: rx=%lu miss=%lu dup=%lu -> %.2f%%   lastPkt rssi=%.1fdBm snr=%.2fdB"
       "  ago=%lums\n",
       (unsigned long)g_seqRx, (unsigned long)g_seqMissing, (unsigned long)g_seqDup,
       denom ? (100.0 * (double)g_seqMissing / (double)denom) : 0.0,
       -1.0 * (double)g_lastRssiPkt / 2.0, (double)g_lastSnrPkt / 4.0,
       (unsigned long)(millis() - g_lastPktMs));
  }
}

// ---------------------------------------------------------------- 底噪扫描
static void noiseScan() {
  pc("#noise scan 460..480MHz step 2MHz (set freq -> RX -> 40ms -> avg 4 samples)\n");
  const double saved = g_freqMHz;
  for (int f = 460; f <= 480; f += 2) {
    cmdSetStandby();
    g_freqMHz = (double)f;
    const uint32_t frf = rfFreqWord();
    uint8_t df[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8),
                     (uint8_t)frf};
    cmdWrite(CMD_SET_RF_FREQUENCY, df, 4);
    cmdSetRx(RX_TIMEOUT_CONT);
    delay(40);
    int32_t acc = 0;
    for (uint8_t i = 0; i < 4; i++) { acc += (int32_t)cmdRssiInst(); delay(2); }
    pc("#  %dMHz  rssi=%.1fdBm\n", f, -1.0 * (double)acc / 4.0 / 2.0);
  }
  g_freqMHz = saved;
  configure(false);
  pc("#scan done, back to %.3fMHz RX\n", g_freqMHz);
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
  pc("  a          print one report now\n");
  pc("  l          per-second report on/off\n");
  pc("  r          re-run config (standby..SetRx) and re-enter RX\n");
  pc("  f<MHz>     change frequency, e.g. f470.5  -> reconfig + RX\n");
  pc("  n          noise scan 460..480MHz (RSSI per 2MHz)\n");
  pc("  s          SPI clock: 1M -> 2M -> 4M -> 8M -> 16M MHz\n");
  pc("  0740 / 0320.16   read register(s), with raw bytes\n");
  pc("  h          help\n");
}

static void stepClock() {
  static const uint32_t hz[5] = {1000000UL, 2000000UL, 4000000UL, 8000000UL, 16000000UL};
  uint8_t i = 0;
  for (i = 0; i < 5; i++) if (hz[i] == g_hz) break;
  g_hz = hz[(i + 1) % 5];
  pc("#clk = %lukHz (press r to re-enter RX)\n", (unsigned long)(g_hz / 1000));
}

static char s_line[32];
static uint8_t s_len = 0;

static void onLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;
  const char c = line[0];
  if (c == 'a') { report(); return; }
  if (c == 'l') { g_auto = !g_auto; pc("#per-second report = %s\n", g_auto ? "ON" : "OFF"); return; }
  if (c == 'r') { configure(true); report(); return; }
  if (c == 'n') { noiseScan(); return; }
  if (c == 's') { stepClock(); return; }
  if (c == 'h' || c == '?') { printHelp(); return; }
  if (c == 'f') {
    const double f = atof(line + 1);
    if (f < 400.0 || f > 520.0) { pc("#? frequency out of range (400..520 MHz)\n"); return; }
    g_freqMHz = f;
    configure(true);
    pc("#freq = %.3fMHz, re-entered RX\n", g_freqMHz);
    report();
    return;
  }
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

  pc("\n===== lora-rx : SX1268 LoRa RX only (470MHz SF7 BW500k) =====\n");
  pc("board : %s\n", BOARD_NAME);
  pc("spi   : NSS=G%d SCK=G%d MOSI=G%d MISO=G%d  mode0  %lukHz\n", (int)PIN_NSS,
     (int)PIN_SCK, (int)PIN_MOSI, (int)PIN_MISO, (unsigned long)(g_hz / 1000));
  pc("peer  : flash lora-tx on the other board (same RF settings)\n");

  digitalWrite(PIN_RST, LOW);
  delay(RST_LOW_MS);
  digitalWrite(PIN_RST, HIGH);
  delay(5);

  configure(true);
  pc("#verify: pkt=0x01(LoRa) st=RX err=0. Noise floor with nothing transmitting\n");
  pc("#should sit around -100..-125dBm and FLUCTUATE. A rock-steady value means the\n");
  pc("#RF switch is not switching (check the 0x9D 01 command).\n");
  report();
  printHelp();
  g_lastReport = millis();
}

void loop() {
  const uint32_t now = millis();
  pollSerial();
  pollRx();

  if (g_auto && (uint32_t)(now - g_lastReport) >= REPORT_MS) {
    g_lastReport = now;
    report();
  }
  delay(2);
}
