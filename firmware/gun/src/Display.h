#pragma once
#include <Arduino.h>
#include <U8g2lib.h>
#include "GunState.h"
#include "Menu.h"

// 枪端显示屏：SSD1306 OLED 128x64 I2C（SW_I2C 支持任意 SDA/SCL 引脚）
// 显示：血量/弹药大字、队伍、玩家 ID、状态（存活/阵亡/装弹/重生倒计时）
// 菜单激活时覆盖显示菜单（4 按钮导航）
class GunDisplay {
 public:
  void begin(uint8_t sda, uint8_t scl, uint8_t addr);

  // 定期调用（建议每 200ms）：按当前状态刷新
  void update(bool registered, const char *deviceId);

 private:
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C *_u8g2 = nullptr;
  unsigned long _lastDraw = 0;
  int _lastHp = -1, _lastAmmo = -1;
  bool _lastAlive = false;

  void drawIdle(const char *deviceId);
  void drawCombat(const char *deviceId);
  void drawMenu();   // 菜单覆盖层（含主菜单/武器/功率/信息/重启）
};

extern GunDisplay display;
