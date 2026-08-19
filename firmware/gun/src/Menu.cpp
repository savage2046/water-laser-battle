#include "Menu.h"
#include "config.h"

Menu menu;

void Menu::begin() {
  _state = IDLE;
  _sel = 0;
  _pending = NONE;
}

Menu::Action Menu::takeAction() {
  Action a = _pending;
  _pending = NONE;
  return a;
}

void Menu::onUp() {
  _lastInput = millis();
  switch (_state) {
    case MAIN:
      if (_sel > 0) _sel--;
      break;
    default:
      break;
  }
}

void Menu::onDown() {
  _lastInput = millis();
  switch (_state) {
    case MAIN:
      if (_sel < MAIN_ITEMS - 1) _sel++;
      break;
    default:
      break;
  }
}

void Menu::onOk() {
  _lastInput = millis();
  switch (_state) {
    case IDLE:
      _state = MAIN;
      _sel = 0;
      break;
    case MAIN:
      switch (_sel) {
        case 0: _pending = PAIR_HELMET; _state = IDLE; break;  // 连接头盔：触发配对
        case 1: _pending = JOIN_MATCH;  _state = IDLE; break;  // 加入战斗：触发重连
        case 2: _state = CALIBRATE; break;                     // 校准激光：进入校准
      }
      break;
    case CALIBRATE:
      _state = IDLE;  // 校准中确认：退出
      break;
    default:
      break;
  }
}

void Menu::onCancel() {
  _lastInput = millis();
  switch (_state) {
    case IDLE:
      break;
    case MAIN:
      _state = IDLE;   // 主菜单取消 = 退出
      break;
    case CALIBRATE:
      _state = IDLE;   // 校准中取消 = 退出
      break;
    default:
      break;
  }
}

// 无操作超时：返回待机（防战斗中误触菜单挡显示）
void Menu::update() {
  if (_state != IDLE && millis() - _lastInput > MENU_TIMEOUT_MS) {
    _state = IDLE;
    _sel = 0;
  }
}
