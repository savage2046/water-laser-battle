#include "RadioLink.h"
#include "config.h"
#include <stdarg.h>

RadioLink radio;

void RadioLink::begin() {
  // U2 = Ra-01S（SX1268 芯片）挂 ESP32-S3 硬件 SPI（引脚见 config.h PIN_SX_*）
  SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);
  // ⚠️ 必须用 SX1268 类：RadioLib 用版本字符串校验身份（SX1268 类只认 "SX1268"），
  //    用 SX1262 类（只认 "SX1261"）会 10 次重试后返回 -2 CHIP_NOT_FOUND。
  SX1268 *p = new SX1268(new Module(PIN_SX_NSS, PIN_SX_DIO1, PIN_SX_RST, PIN_SX_BUSY));
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
