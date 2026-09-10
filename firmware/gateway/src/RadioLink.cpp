#include "RadioLink.h"
#include "config.h"

bool RadioLink::begin(float freqMhz) {
  // SPI 总线共享：首次调用初始化，后续实例复用（引脚相同，NSS 各管各的）
  static bool spiInit = false;
  if (!spiInit) {
    SPI.begin(PIN_SX_SCLK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);
    spiInit = true;
  }

  // ⚠️ 必须用 SX1268 类（Ra-01S 芯片）：RadioLib 用版本字符串校验身份，
  //    用 SX1262 类（只认 "SX1261"）会重试 10 次后返回 -2 CHIP_NOT_FOUND。
  SX1268 *p = new SX1268(new Module(_nss, _dio1, _rst, _busy));
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
  return true;
}
