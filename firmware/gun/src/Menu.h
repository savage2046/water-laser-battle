#pragma once
#include <Arduino.h>

// 枪端简化菜单：三个功能项（4 按钮：上/下/确认/取消）
//   ① 连接头盔  —— 触发配对：服务器向头盔同步本枪玩家 ID
//   ② 加入战斗  —— 手动触发注册/重连服务器
//   ③ 校准激光  —— 进入校准模式（参数由服务器 W 帧下发，枪端无输入）
class Menu {
 public:
  enum State { IDLE, MAIN, CALIBRATE };
  // 主菜单项动作（main.cpp 消费）
  enum Action { NONE, PAIR_HELMET, JOIN_MATCH };

  void begin();
  void onUp();
  void onDown();
  void onOk();
  void onCancel();
  void update();           // 每循环：超时返回

  bool active() { return _state != IDLE; }
  bool calibrating() { return _state == CALIBRATE; }
  State state() { return _state; }
  uint8_t sel() { return _sel; }

  // 消费一次主菜单动作（确认选中"连接头盔"/"加入战斗"时返回）
  Action takeAction();

 private:
  static const uint8_t MAIN_ITEMS = 3;
  State _state = IDLE;
  uint8_t _sel = 0;
  Action _pending = NONE;
  unsigned long _lastInput = 0;
};

extern Menu menu;
