#include <Arduino.h>
#include <esp_timer.h>

#include "config.h"

// ============================================================================
// wire-probe —— 开机后 4 个管脚持续输出固定频率方波（不用 SPI）
//
//     NSS(G16)  =   10 Hz
//     SCK(G42)  =  100 Hz
//     MOSI(G15) =  500 Hz
//     RST(G46)  = 1000 Hz
//
// 四路都由同一个 4kHz 硬件定时器分频产生：相位对齐、频率精确、不受主循环影响。
// 用示波器把探头点在上述管脚（或对应的模组焊盘）上，就能看这 4 个频率。
// ============================================================================

// ===== 调试开关 =============================================================
// G46(RST) 这一路如果频率/波形不对，把下面改成 1 重新编译烧进去：
//   RST 改成输出 2Hz（其余三根 10/100/500Hz 不变）。
//   判读：2Hz 仍然乱或没信号 -> 这一路是断线/虚焊/接触不良（与频率无关）
//         2Hz 干净、1kHz 才乱 -> 这一路有容性负载/串阻（RC 把方波积分成三角波）
#define RST_SLOW_TEST 0

// 引脚、周期（tick 数）、频率 —— 一一对应：250µs × 周期 = 一个完整周期
static const uint8_t  kPin[4]     = {PIN_NSS, PIN_SCK, PIN_MOSI, PIN_RST};
static const char    *kName[4]    = {"NSS ", "SCK ", "MOSI", "RST "};
static const uint32_t kPeriod[4]  = {400, 40, 8,
#if RST_SLOW_TEST
                                     2000          // 2 Hz
#else
                                     4             // 1000 Hz
#endif
};
static const uint32_t kHz[4]      = {10, 100, 500,
#if RST_SLOW_TEST
                                     2
#else
                                     1000
#endif
};

static volatile uint32_t g_tick = 0;
static volatile uint8_t  g_lvl[4] = {0, 0, 0, 0};

// 每个 tick：高电平占前半个周期，低电平占后半个周期（50% 占空比）
static void IRAM_ATTR onTick(void *) {
  const uint32_t t = ++g_tick;
  for (uint8_t i = 0; i < 4; i++) {
    const uint32_t per = kPeriod[i];
    const uint8_t v = (uint8_t)((t % per) < (per / 2) ? 1 : 0);
    if (v != g_lvl[i]) {
      g_lvl[i] = v;
      digitalWrite(kPin[i], v ? HIGH : LOW);
    }
  }
}

void setup() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(kPin[i], OUTPUT);
    digitalWrite(kPin[i], LOW);
  }
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, (LED_ON_LEVEL == HIGH) ? LOW : HIGH);   // 板载 LED 关掉，免得干扰

  Serial.begin(115200);
  delay(200);
  Serial.printf("wire-probe : 4 pins, fixed square waves\nboard: %s\n", BOARD_NAME);
  for (uint8_t i = 0; i < 4; i++)
    Serial.printf("  %s G%-3d = %lu Hz\n", kName[i], (int)kPin[i],
                  (unsigned long)kHz[i]);

  esp_timer_create_args_t args = {};
  args.callback = &onTick;
  args.name = "pinwave";
  args.dispatch_method = ESP_TIMER_TASK;
  esp_timer_handle_t t = nullptr;
  esp_timer_create(&args, &t);
  esp_timer_start_periodic(t, TICK_US);      // 250µs → 4kHz
}

void loop() {
  // 什么都不做：波形全部由定时器产生
}
