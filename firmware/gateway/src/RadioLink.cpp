#include "RadioLink.h"
#include "config.h"

bool RadioLink::begin(float freqMhz) {
  // SPI 总线共享：首次调用初始化，后续实例复用（引脚相同，NSS 各管各的）
  static bool spiInit = false;
  if (!spiInit) {
    SPI.begin(SX_SCLK, SX_MISO, SX_MOSI, SX_NSS);
    spiInit = true;
  }

  _radio = new SX1262(new Module(_nss, _dio1, _rst, _busy));
  _radio->setSPI(&SPI);

  int state = _radio->begin(freqMhz, RADIO_BW_KHZ, RADIO_SF, RADIO_CR,
                            RADIO_SYNC_WORD, RADIO_TX_POWER_DBM,
                            RADIO_PREAMBLE_LEN, 0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[radio] SX1262 init failed (%.1fMHz): %d\n", (double)freqMhz,
                  state);
    delete _radio;
    _radio = nullptr;
    _ready = false;
    return false;
  }
  _radio->setPacketMode();
  _radio->startReceive();
  _ready = true;
  Serial.printf("[radio] SX1262 ready %.1fMHz SF%d BW%.0fk\n", (double)freqMhz,
                (int)RADIO_SF, (double)RADIO_BW_KHZ);
  return true;
}
