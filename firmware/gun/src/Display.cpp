#include "Display.h"
#include "config.h"

GunDisplay display;

// 环境光等级名（main.cpp 定义：LOW/MID/HI）
extern const char *lightName();

// 队伍名
static const char *teamName(int16_t t) {
  switch (t) {
    case 0: return "RED";
    case 1: return "BLU";
    default: return "FFA";
  }
}

void GunDisplay::begin(uint8_t sda, uint8_t scl, uint8_t addr) {
  _u8g2 = new U8G2_SSD1306_128X64_NONAME_F_SW_I2C(U8G2_R0, scl, sda,
                                                   U8X8_PIN_NONE);
  _u8g2->setI2CAddress(addr << 1);  // U8g2 需要 8 位地址（0x3C → 0x78）
  _u8g2->begin();
  _u8g2->clear();
  _u8g2->setFont(u8g2_font_6x10_tf);
  _u8g2->drawStr(20, 30, "GUN BOOT...");
  _u8g2->sendBuffer();
}

void GunDisplay::update(bool registered, const char *deviceId) {
  // 菜单激活：覆盖显示（200ms 节流 + 状态变化刷新）
  if (menu.active()) {
    if (millis() - _lastDraw < 200) return;
    _lastDraw = millis();
    drawMenu();
    return;
  }
  // 节流：200ms 一帧；血量/弹药/状态变化时立即刷新
  bool dirty = (gun.hp() != _lastHp) || (gun.ammo() != _lastAmmo) ||
               (gun.isAlive() != _lastAlive);
  if (!dirty && millis() - _lastDraw < 200) return;
  _lastDraw = millis();
  _lastHp = gun.hp();
  _lastAmmo = gun.ammo();
  _lastAlive = gun.isAlive();

  if (!registered) {
    drawIdle(deviceId);
  } else {
    drawCombat(deviceId);
  }
}

// 未注册：等待服务器配置
void GunDisplay::drawIdle(const char *deviceId) {
  _u8g2->clearBuffer();
  _u8g2->setFont(u8g2_font_6x10_tf);
  _u8g2->drawStr(10, 20, "GUN  WAITING");
  _u8g2->drawStr(10, 36, deviceId);
  _u8g2->drawStr(10, 52, "JOIN MATCH...");
  _u8g2->sendBuffer();
}

// 对局中/阵亡/装弹
void GunDisplay::drawCombat(const char *deviceId) {
  _u8g2->clearBuffer();

  // 行1：玩家 + 队伍 + 存活 + 弹药 + 环境光（户外强光提示）
  char top[40];
  snprintf(top, sizeof(top), "P%u %s %s AM%d %s",
           gun.playerId(), teamName(gun.team()),
           gun.isAlive() ? "ALIVE" : "DEAD", gun.ammo(), lightName());
  _u8g2->setFont(u8g2_font_6x10_tf);
  _u8g2->drawStr(2, 10, top);
  _u8g2->drawStr(2, 20, deviceId);

  // 血量进度条
  int barW = 124;
  long fill = (long)gun.hp() * barW / max(gun.maxHp(), 1);
  _u8g2->drawFrame(2, 26, barW, 8);
  if (fill > 1) _u8g2->drawBox(3, 27, (int)fill - 1, 6);

  // 血量大字
  char hp[12];
  snprintf(hp, sizeof(hp), "%d", gun.hp());
  _u8g2->setFont(u8g2_font_fur20_tn);
  _u8g2->drawStr(2, 52, hp);
  int hw = _u8g2->getStrWidth(hp);
  _u8g2->setFont(u8g2_font_6x10_tf);
  _u8g2->drawStr(hw + 8, 52, "HP");

  // 底部状态：装弹 / 重生倒计时 / 锁 / 就绪
  char st[32];
  if (gun.isReloading()) {
    snprintf(st, sizeof(st), "RELOADING");
  } else if (!gun.isAlive()) {
    unsigned long age = gun.deathAgeMs();
    long remain = (long)gun.respawnMs() - (long)age;
    if (remain < 0) remain = 0;
    snprintf(st, sizeof(st), "RESPAWN %lds", (remain + 999) / 1000);
  } else if (gun.isLocked()) {
    snprintf(st, sizeof(st), "LOCKED");
  } else {
    snprintf(st, sizeof(st), "READY");
  }
  _u8g2->drawStr(2, 62, st);
  _u8g2->sendBuffer();
}

// 菜单渲染（覆盖战斗界面）：三功能项 / 校准模式
void GunDisplay::drawMenu() {
  _u8g2->clearBuffer();
  _u8g2->setFont(u8g2_font_6x10_tf);

  auto drawItem = [&](uint8_t y, const char *text, bool sel) {
    if (sel) _u8g2->drawBox(0, y - 9, 128, 11);
    _u8g2->setDrawColor(sel ? 0 : 1);
    _u8g2->drawStr(4, y, text);
    _u8g2->setDrawColor(1);
  };

  switch (menu.state()) {
    case Menu::MAIN: {
      _u8g2->drawStr(2, 9, "== MENU ==");
      const char *items[3] = { "Pair Helmet", "Join Match", "Calibrate" };
      for (uint8_t i = 0; i < 3; i++) {
        drawItem(24 + i * 12, items[i], menu.sel() == i);
      }
      break;
    }
    case Menu::CALIBRATE: {
      // 校准模式：扣扳机发测试帧（参数由服务器 W 帧下发）
      _u8g2->drawStr(2, 12, "== CALIBRATE ==");
      _u8g2->drawStr(2, 28, "aim at target");
      _u8g2->drawStr(2, 42, "pull trigger");
      _u8g2->drawStr(2, 56, "cancel to exit");
      break;
    }
    default:
      break;
  }
  _u8g2->sendBuffer();
}
