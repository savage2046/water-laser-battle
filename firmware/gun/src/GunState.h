#pragma once
#include <Arduino.h>
#include "LaserCodec.h"

// 枪械状态机：血量/弹药/装弹/阵亡/重生/扳机锁。
// 血量权威在本机：红外命中本地扣血，无线断链不影响判定。
class GunState {
 public:
  void begin();

  // 应用服务器规则（W 帧 / welcome）
  void applyConfig(uint16_t playerId, int16_t team, const uint16_t *friends,
                   uint8_t friendCount, int hp, int dmg, int ammo,
                   uint32_t reloadMs, uint32_t respawnMs, uint8_t scoreToWin);
  void resetForMatch();          // S 帧：满血满弹解锁
  void endMatch(int winner);     // E 帧：锁扳机等待下一局
  void respawn();                // R 帧：满血满弹解锁
  void pause() { _locked = true; }
  void resume() { _locked = false; }

  bool canFire();                // 存活 && 未锁 && 有弹 && 未装弹
  void consumeShot();            // 扣弹药，空弹触发装弹

  // 每循环调用：处理装弹计时等时间相关状态
  void update();

  // 被红外命中：返回 true 表示本次扣血；killed 表示阵亡。
  // 按 (playerId, shotSeq) 精确去重：同一射击帧无论几路接收头收到只扣一次血
  bool applyHit(const LaserFrame &f, bool &killed);

  // 外部命中（头盔 X 帧转发，带 shotSeq）：按射手+时间窗去重（500ms）
  bool applyExternalHit(uint16_t shooter, uint8_t weapon, uint8_t shotSeq,
                        bool &killed);

  bool isFriend(uint16_t id);
  bool isAlive() { return _alive; }
  bool isLocked() { return _locked; }
  bool isReloading() { return _reloading; }
  int hp() { return _hp; }
  int maxHp() { return _maxHp; }
  int ammo() { return _ammo; }
  int maxAmmo() { return _maxAmmo; }
  uint16_t playerId() { return _playerId; }
  int16_t team() { return _team; }
  uint16_t lastShooter() { return _lastShooter; }
  uint8_t lastWeapon() { return _lastWeapon; }
  uint32_t respawnMs() { return _respawnMs; }

  // 装弹剩余毫秒（未装弹时返回 0）
  unsigned long reloadRemainMs() {
    if (!_reloading) return 0;
    long remain = (long)_reloadUntil - (long)millis();
    return remain > 0 ? (unsigned long)remain : 0;
  }

  // 阵亡至今毫秒（存活时返回 0）
  unsigned long deathAgeMs() {
    if (_alive || _diedAt == 0) return 0;
    return millis() - _diedAt;
  }

  // 状态变化辅助（喂蜂鸣器/LED）
  enum Event { EV_HIT, EV_DEATH, EV_RESPAWN, EV_RELOAD, EV_MATCH_START };
  Event lastEvent() { return _lastEvent; }

 private:
  void doDamage(uint16_t shooter, uint8_t weapon, bool &killed);
  bool hitDedup(uint16_t shooter, uint8_t shotSeq);

  uint16_t _playerId = 0;
  int16_t _team = 0;
  uint16_t _friends[8];
  uint8_t _friendCount = 0;

  int _hp = 0, _maxHp = 0, _dmg = 0;
  int _ammo = 0, _maxAmmo = 0;
  uint32_t _reloadMs = 2000, _respawnMs = 3000;
  uint8_t _scoreToWin = 50;

  bool _alive = false;
  bool _locked = true;      // 对局未开始/暂停/阵亡时锁定扳机
  bool _reloading = false;
  unsigned long _reloadUntil = 0;
  unsigned long _diedAt = 0;    // 阵亡时刻（重生倒计时显示用）

  // 命中去重表：(shooter, shotSeq) 最近 16 条
  struct HitKey { uint16_t shooter; uint8_t seq; };
  HitKey _dedup[16];
  uint8_t _dedupIdx = 0;

  // 外部命中（无 seq）时间窗去重
  uint16_t _extShooter = 0;
  unsigned long _extAt = 0;

  uint16_t _lastShooter = 0;
  uint8_t _lastWeapon = 0;
  Event _lastEvent = EV_MATCH_START;
};

extern GunState gun;
