#include "RadioLink.h"
#include "config.h"

// ============================================================================
// 网关多射频版 LoRa 初始化（RadioLib SX126x 驱动）。
// 以下三条**必发命令**少任何一条都会出现"看起来全对但就是不通"的假象
// （2026-09 逐条实测确认，细节与现象见 gun/src/RadioLink.cpp 顶部同段注释）：
//
//   ① SetDio2AsRfSwitchCtrl(0x9D,1)  DIO2 驱动模组 RF 开关（Ra-01S 的 TXEN/RXEN）——
//      本板每个射频只连了 6 根线（无 TXEN/RXEN）。漏发：发射无辐射、RSSI 卡死不变。
//      → RadioLib SX126x::begin() 固定发（SX126x.cpp:192）。
//   ② SetDioIrqParams(0x08)  解除 IRQ 屏蔽（§13.3.2 默认全屏蔽，GetIrqStatus 恒 0）——
//      TdmaMac::readPacketPoll() 靠轮询 IRQ 寄存器，漏发就永远等不到 RxDone/TxDone。
//      → RadioLib 每次收发前重设（startReceiveCommon / startTransmit）。
//      ⚠️ 不要绕过 startReceive()/startTransmit() 直接 setRx()/setTx()。
//   ③ SetPaConfig(0x95)+SetTxParams(0x8E)  PA 与功率（表 13-21：0x04/0x07/0x00/0x01）——
//      → RadioLib begin() 的 setOutputPower(RADIO_TX_POWER_DBM) 完成。
//
// 另外：多射频共用 SPI，SCLK/MOSI/MISO 相同、NSS 各自选通；DIO1 只有槽 0 有，
//       其余槽 dio1=-1（**不要对它们用阻塞 transmit()**：它靠 DIO1 电平判完成，
//       dio1=-1 时会白等 5×空口 ≈46ms；发射一律走 TdmaMac 的 startTransmit+轮询）。
// ============================================================================

bool RadioLink::begin(float freqMhz) {
  // SPI 总线共享：首次调用初始化，后续实例复用（引脚相同，NSS 各管各的）
  static bool spiInit = false;
  if (!spiInit) {
    SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);
    spiInit = true;
  }

  // ⚠️ 必须用 SX1268 类（Ra-01S 芯片）：RadioLib 用版本字符串校验身份，
  //    用 SX1262 类（只认 "SX1261"）会重试 10 次后返回 -2 CHIP_NOT_FOUND。
  Module *m = new Module(_nss, _dio1, _rst, _busy);
  _mod = m;                      // 自检时用它直接读寄存器（见 selfCheck）
  SX1268 *p = new SX1268(m);
  // RadioLib 6.x 已移除 setSPI()：默认使用全局 SPI 实例（引脚由上方 SPI.begin 配置）
  // ⚠️ 不要写 _radio->setSPI(&SPI)：6.5.0 无此接口，编译报
  //    "error: 'class SX1262' has no member named 'setSPI'"

  int state = p->begin(freqMhz, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                       RADIO_SYNC_WORD, RADIO_TX_POWER_DBM,
                       RADIO_PREAMBLE_LEN, 0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[radio] SX1268 init failed (%.1fMHz): %d"
                  "（-2=CHIP_NOT_FOUND：类不匹配/焊接/供电）\n",
                  (double)freqMhz, state);
    delete p;
    _radio = nullptr;
    _ready = false;
    return false;
  }
  _radio = p;   // 上转为基类指针 SX126x*（TdmaMac 用）
  // （RadioLib 6.x 的 setPacketMode(mode,len) 仅适用 GFSK 调制，本项目为 LoRa，
  //   调用会返回 RADIOLIB_ERR_WRONG_MODEM；旧版无参调用已删除，不再调用）
  _radio->startReceive();
  _ready = true;
  Serial.printf("[radio] SX1268 ready %.1fMHz SF%d BW%.0fk\n", (double)freqMhz,
                (int)RADIO_SF, (double)RADIO_BW_KHZ);
  selfCheck();
  return true;
}

// 开机自检：把"这个射频的 SPI/芯片/网络参数到底对不对"变成开机日志里的硬判据
// 说明：begin() 成功已隐含"版本串匹配 SX1268"（RadioLib findChip 校验过）；这里额外打印
//       版本串与同步字留作对照，并覆盖 begin() 不检查的两点：芯片错误标志（PLL/校准失败
//       不会让 begin() 失败）与网络同步字是否真的写进去。
bool RadioLink::selfCheck() {
  if (!_ready || _radio == nullptr || _mod == nullptr) {
    Serial.printf("[radio] selfCheck(nss=%u): radio not ready\n", (unsigned)_nss);
    return false;
  }
  uint8_t ver[16] = {0};
  uint8_t sync[2] = {0, 0};
  uint8_t de[2] = {0, 0};
  uint8_t pkt = 0xFF;
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
  const float rssi = _radio->getRSSI(false);
  Serial.printf("[radio] selfCheck nss=%u ver=\"%s\"%s sync=0x%02X%02X%s pkt=%u(%s) "
                "err=0x%04X rssi=%.1fdBm\n",
                (unsigned)_nss, asc, verOk ? "" : " <-- 不是 SX126x！查 SPI/供电/复位",
                sync[0], sync[1],
                syncOk ? "" : " <-- 非私有网 0x1424（RADIO_SYNC_WORD 应配 0x12）",
                (unsigned)pkt, (pkt == 0x01) ? "LoRa" : "(应为 1=LoRa)",
                err, (double)rssi);
  return verOk && syncOk && (err == 0);
}