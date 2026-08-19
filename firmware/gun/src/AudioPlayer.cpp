#include "AudioPlayer.h"
#include <esp_system.h>
#include "driver/i2s.h"
#include <math.h>

AudioPlayer audio;

static uint32_t synthTone(int16_t *buf, uint32_t p, uint32_t n, float f0,
                          float f1, float amp) {
  float phase = 0;
  for (uint32_t i = 0; i < n; i++) {
    float t = (float)i / n;
    float f = f0 + (f1 - f0) * t;
    float env = sinf(3.14159265f * t);
    buf[p + i] = (int16_t)(amp * env * sinf(phase));
    phase += 2 * 3.14159265f * f / 8000.0f;
  }
  return p + n;
}

static uint32_t synthNoise(int16_t *buf, uint32_t p, uint32_t n, float amp) {
  for (uint32_t i = 0; i < n; i++) {
    float t = (float)i / n;
    float env = expf(-4.0f * t);
    buf[p + i] =
        (int16_t)(amp * env * (int32_t)(esp_random() & 0xFFFF) / 32767.0f * 0.7f);
  }
  return p + n;
}

void AudioPlayer::begin(uint8_t bclk, uint8_t ws, uint8_t dout) {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 8000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 128;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = bclk;
  pins.ws_io_num = ws;
  pins.data_out_num = dout;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t e = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if (e != ESP_OK) {
    Serial.printf("[audio] i2s install failed: %d\n", e);
    return;
  }
  i2s_set_pin(I2S_NUM_0, &pins);
  _ok = true;
  _len = 0;
  _pos = 0;
}

void AudioPlayer::play(Fx fx) {
  if (!_ok) return;
  _pos = 0;
  switch (fx) {
    case FX_SHOT:    _len = synthNoise(_buf, 0, 1200, 16000); break;
    case FX_HIT:     _len = synthTone(_buf, 0, 800, 1200, 1200, 14000); break;
    case FX_DEATH:   _len = synthTone(_buf, 0, 2400, 800, 200, 16000); break;
    case FX_RESPAWN: _len = synthTone(_buf, 0, 2000, 300, 900, 15000); break;
    case FX_START: {
      uint32_t p = synthTone(_buf, 0, 800, 880, 880, 14000);
      _len = synthTone(_buf, p, 800, 1320, 1320, 14000);
      break;
    }
    case FX_END:     _len = synthTone(_buf, 0, 3200, 440, 440, 15000); break;
    case FX_CLICK:   _len = synthTone(_buf, 0, 320, 2000, 2000, 12000); break;
    default:         _len = 0; break;
  }
}

void AudioPlayer::update() {
  if (!_ok || _pos >= _len) return;
  size_t written = 0;
  uint32_t chunk = min((uint32_t)256, _len - _pos);
  i2s_write(I2S_NUM_0, &_buf[_pos], chunk * sizeof(int16_t), &written, 0);
  _pos += written / sizeof(int16_t);
}
