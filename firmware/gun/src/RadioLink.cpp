#include "RadioLink.h"
#include "config.h"
#include <stdarg.h>

RadioLink radio;

void RadioLink::begin() {
  // E22-400M22S（SX1268）挂 ESP32-S3 硬件 SPI（引脚见 config.h PIN_SX_*）
  SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);
  _radio = new SX1262(new Module(PIN_SX_NSS, PIN_SX_DIO1, PIN_SX_RST, PIN_SX_BUSY));
  // RadioLib 6.x 默认使用全局 SPI 实例（已由上方 SPI.begin 配置引脚），
  // 旧版 setSPI() 接口已移除，不再调用。

  int state = _radio->begin(RADIO_FREQ_MHZ, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                            RADIO_SYNC_WORD, RADIO_TX_POWER_DBM,
                            RADIO_PREAMBLE_LEN, 0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[radio] SX1262 init failed: %d\n", state);
    _ready = false;
    return;
  }
  // （RadioLib 6.x 的 setPacketMode(mode,len) 仅适用 GFSK 调制，本项目为 LoRa，
  //   调用会返回 RADIOLIB_ERR_WRONG_MODEM 且无副作用——旧版无参调用已删除）
  _radio->startReceive();
  _ready = true;
  Serial.printf("[radio] SX1262 ready %.1fMHz SF%d BW%.0fk\n",
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
  // 检查是否收到包（非阻塞）
  int state = _radio->readData((uint8_t *)_buf, sizeof(_buf) - 1);
  if (state == RADIOLIB_ERR_NONE) {
    int len = _radio->getPacketLength();
    if (len > 0 && (size_t)len < maxLen) {
      memcpy(out, _buf, len);
      out[len] = '\0';
      return true;
    }
  } else if (state != RADIOLIB_ERR_RX_TIMEOUT && state != RADIOLIB_ERR_NONE) {
    // 非超时错误：重启接收
    _radio->startReceive();
  }
  return false;
}
