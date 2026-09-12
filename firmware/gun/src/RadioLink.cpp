#include "RadioLink.h"
#include "config.h"
#include <stdarg.h>

// ============================================================================
// 本文件的 LoRa 初始化依赖 RadioLib 的 SX126x 驱动。下面三条是**必发命令**，
// 少任何一条都会出现"看起来全对但就是不通"的假象（2026-09 逐条实测确认）：
//
//   ① SetDio2AsRfSwitchCtrl(0x9D, 1) —— DIO2 驱动模组的 RF 开关
//      Ra-01S 的收发切换靠 TXEN(脚5)/RXEN(脚11)，由 DIO2 驱动（手册表 13-33：
//      RX 模式 DIO2=0、TX 模式 DIO2=1）。本板 U2 只连了 6 根线（没有 TXEN/RXEN），
//      所以只能靠 DIO2。**漏发**：发射没辐射、接收看到被隔离的前端（RSSI 卡死不变）。
//      → RadioLib 在 SX126x::begin() 里固定发（SX126x.cpp:192）。
//
//   ② SetDioIrqParams(0x08) —— 解除 IRQ 屏蔽
//      手册 §13.3.2：默认**所有 IRQ 被屏蔽（全 0）**，不解除则 GetIrqStatus 恒为 0x0000。
//      **漏发**：本文件的 poll() 与 TdmaMac::readPacketPoll() 都靠轮询 IRQ 寄存器，
//      会永远等不到 RxDone/TxDone（实测现象：irq=0x0000，而状态字节已报 "TX done"）。
//      → RadioLib 每次收发前都会重设：startReceive() 用 RX_DONE|CRC_ERR|HEADER_ERR(+TIMEOUT)，
//        startTransmit() 用 TX_DONE|TIMEOUT（SX126x.cpp startReceiveCommon/startTransmit）。
//        ⚠️ 因此**不要绕过** startReceive()/startTransmit() 直接 setRx()/setTx()，否则屏蔽态未开。
//
//   ③ SetPaConfig(0x95) + SetTxParams(0x8E) —— PA 配置与输出功率
//      不配 = 默认低功率 PA（+14dBm 档）。高功率档见手册表 13-21：
//      paDutyCycle=0x04 hpMax=0x07 deviceSel=0x00 paLut=0x01。
//      → RadioLib 在 begin() 里通过 setOutputPower(RADIO_TX_POWER_DBM) 完成
//        （SX1268.cpp: setPaConfig(0x04, deviceSel=0x00) + setTxParams(power, ramp=200µs)）。
//
//  另外两条实测结论：
//    · SPI 读寄存器时，命令/地址之后先有一个 **Status 字节**，数据在它后面
//      （手册表 13-25：数据从第 4 字节起；RadioLib 的 buffIn[cmdLen+1] 同理）。
//    · 复位后默认 pktType=FSK、寄存器为表 12-1 的复位值（0x0740=0x14 / 0x0741=0x24 /
//      0x08AC=0x94 / 0x0911=0x05 / 0x0912=0x05），可用来校验"SPI 到底读对没有"。
// ============================================================================

RadioLink radio;

void RadioLink::begin() {
  // U2 = Ra-01S（SX1268 芯片）挂 ESP32-S3 硬件 SPI（引脚见 config.h PIN_SX_*）
  // 先 SPI.begin(明确引脚)：RadioLib 的 Module::init() 再调 SPI.begin() 时会因
  // 总线已初始化而直接返回，所以引脚不会被改成默认值。
  SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);
  // ⚠️ 必须用 SX1268 类：RadioLib 用版本字符串校验身份（SX1268 类只认 "SX1268"），
  //    用 SX1262 类（只认 "SX1261"）会 10 次重试后返回 -2 CHIP_NOT_FOUND。
  Module *m = new Module(PIN_SX_NSS, PIN_SX_DIO1, PIN_SX_RST, PIN_SX_BUSY);
  _mod = m;                      // 自检时用它直接读寄存器（见 selfCheck）
  SX1268 *p = new SX1268(m);
  // RadioLib 6.x 默认使用全局 SPI 实例（已由上方 SPI.begin 配置引脚），
  // 旧版 setSPI() 接口已移除，不再调用。

  int state = p->begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                       RADIO_SYNC_WORD, RADIO_TX_POWER_DBM,
                       RADIO_PREAMBLE_LEN, 0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[radio] SX1268 init failed: %d（-2=CHIP_NOT_FOUND：类不匹配/焊接/供电）\n",
                  state);
    delete p;
    _ready = false;
    return;
  }
  _radio = p;   // 上转为基类指针 SX126x*（TdmaMac 用）
  // （RadioLib 6.x 的 setPacketMode(mode,len) 仅适用 GFSK 调制，本项目为 LoRa，
  //   调用会返回 RADIOLIB_ERR_WRONG_MODEM 且无副作用——旧版无参调用已删除）
  _radio->startReceive();
  _ready = true;
  Serial.printf("[radio] SX1268 ready %.1fMHz SF%d BW%.0fk\n",
                (double)RADIO_FREQ_MHZ, (int)RADIO_SF, (double)RADIO_BW_KHZ);
  selfCheck();
}

// 开机自检：把"SPI/芯片/网络参数到底对不对"变成开机日志里的硬判据
// 说明：begin() 成功已经隐含"版本串匹配 SX1268"（RadioLib 的 findChip 校验过），
//       这里额外打印版本串与同步字，是为了留下可对照的记录 + 覆盖 begin() 不检查的两点：
//       芯片错误标志（PLL/校准失败不会让 begin() 失败）与网络同步字是否真的写进去了。
bool RadioLink::selfCheck() {
  if (!_ready || _radio == nullptr || _mod == nullptr) {
    Serial.println("[radio] selfCheck: radio not ready");
    return false;
  }
  uint8_t ver[16] = {0};
  uint8_t sync[2] = {0, 0};
  uint8_t de[2] = {0, 0};
  uint8_t pkt = 0xFF;
  // Module 的公开接口：读寄存器（数据从第 4 字节起，RadioLib 会自动跳掉 Status 字节）
  _mod->SPIreadRegisterBurst(RADIOLIB_SX126X_REG_VERSION_STRING, 16, ver);
  _mod->SPIreadRegisterBurst(RADIOLIB_SX126X_REG_LORA_SYNC_WORD_MSB, 2, sync);
  _mod->SPIreadStream(RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS, de, 2);   // OpError(15:0)
  _mod->SPIreadStream(RADIOLIB_SX126X_CMD_GET_PACKET_TYPE, &pkt, 1);  // 1=LoRa / 0=FSK
  const uint16_t err = (uint16_t)(((uint16_t)de[0] << 8) | de[1]);

  char asc[17];
  for (uint8_t i = 0; i < 16; i++)
    asc[i] = (ver[i] >= 0x20 && ver[i] <= 0x7E) ? (char)ver[i] : '.';
  asc[16] = '\0';

  const bool verOk = (memcmp(ver, "SX126", 5) == 0);
  const bool syncOk = (sync[0] == 0x14 && sync[1] == 0x24);
  const float rssi = _radio->getRSSI(false);      // 瞬时 RSSI（RF 前端是否在工作）
  Serial.printf("[radio] selfCheck ver=\"%s\"%s sync=0x%02X%02X%s pkt=%u(%s) "
                "err=0x%04X rssi=%.1fdBm\n",
                asc, verOk ? "" : " <-- 不是 SX126x！查 SPI/供电/复位",
                sync[0], sync[1],
                syncOk ? "" : " <-- 非私有网 0x1424（RADIO_SYNC_WORD 应配 0x12）",
                (unsigned)pkt, (pkt == 0x01) ? "LoRa" : "(应为 1=LoRa)",
                err, (double)rssi);
  if (!verOk)
    Serial.println("[radio]   ver 读不到 → 按序查：① NSS/SCK/MOSI/MISO ② 3.3V 供电 ③ RST(G46) ④ BUSY(G45)");
  if (err != 0)
    Serial.println("[radio]   err 非 0 → PLL/校准报错，查频率参数与晶振");
  return verOk && syncOk && (err == 0);
}


void RadioLink::send(const char *frame) {
  if (!_ready) return;
  // 一帧一包：发完回到连续接收
  _radio->transmit((uint8_t *)frame, strlen(frame));
  _radio->startReceive();
}

void RadioLink::sendFmt(const char *fmt, ...) {
  char tmp[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  send(tmp);
}

bool RadioLink::poll(char *out, size_t maxLen) {
  if (!_ready) return false;
  // 无包时不要调 readData：RadioLib 6.x 在"没有 IRQ 事件"时也会照读缓冲、
  // 返回 RADIOLIB_ERR_NONE，于是把上一包重复吐出来 —— 先查 IRQ 寄存器。
  const uint16_t irq = _radio->getIrqStatus();
  const uint16_t evIrq = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR |
                         RADIOLIB_SX126X_IRQ_HEADER_ERR;
  if (!(irq & evIrq)) return false;

  const bool crcErr =
      (irq & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR)) != 0;
  // RadioLib 文档：getPacketLength() 必须在 readData() 之前调用
  const size_t len = _radio->getPacketLength();
  _radio->readData((uint8_t *)_buf, sizeof(_buf) - 1);   // 读数据 + 清 IRQ
  if (crcErr) {                 // LoRa 硬件 CRC/报头错：丢弃并重新武装
    _radio->startReceive();
    return false;
  }
  if (len > 0 && len < maxLen) {
    memcpy(out, _buf, len);
    out[len] = '\0';
    return true;
  }
  _radio->startReceive();       // 长度异常（同频其它 LoRa 网络）：丢弃
  return false;
}