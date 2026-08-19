#include "GunState.h"
#include "config.h"

GunState gun;

void GunState::begin() {
  _locked = true;
  _alive = false;
  _dedupIdx = 0;
  for (uint8_t i = 0; i < 16; i++) {
    _dedup[i].shooter = 0;
    _dedup[i].seq = 0;
  }
}

void GunState::applyConfig(uint16_t playerId, int16_t team,
                           const uint16_t *friends, uint8_t friendCount,
                           int hp, int dmg, int ammo, uint32_t reloadMs,
                           uint32_t respawnMs, uint8_t scoreToWin) {
  _playerId = playerId;
  _team = team;
  _friendCount = min(friendCount, (uint8_t)8);
  for (uint8_t i = 0; i < _friendCount; i++) _friends[i] = friends[i];
  _maxHp = hp;
  _dmg = dmg;
  _maxAmmo = ammo;
  _reloadMs = reloadMs;
  _respawnMs = respawnMs;
  _scoreToWin = scoreToWin;
}

void GunState::resetForMatch() {
  _hp = _maxHp;
  _ammo = _maxAmmo;
  _alive = true;
  _reloading = false;
  _locked = false;
  _lastEvent = EV_MATCH_START;
}

void GunState::endMatch(int winner) {
  _alive = false;
  _locked = true;
  (void)winner;
}

void GunState::respawn() {
  _hp = _maxHp;
  _ammo = _maxAmmo;
  _alive = true;
  _reloading = false;
  _locked = false;
  _diedAt = 0;
  _lastEvent = EV_RESPAWN;
}

bool GunState::canFire() {
  return _alive && !_locked && _ammo > 0 && !_reloading;
}

void GunState::consumeShot() {
  if (_ammo > 0) _ammo--;
  if (_ammo == 0) {
    _reloading = true;
    _reloadUntil = millis() + _reloadMs;
    _lastEvent = EV_RELOAD;
  }
}

// 处理时间相关状态（装弹计时）
void GunState::update() {
  if (_reloading && millis() >= _reloadUntil) {
    _reloading = false;
    _ammo = _maxAmmo;
  }
}

// 命中去重：同一 (shooter, shotSeq) 最近 16 条内视为重复
bool GunState::hitDedup(uint16_t shooter, uint8_t shotSeq) {
  for (uint8_t i = 0; i < 16; i++) {
    if (_dedup[i].shooter == shooter && _dedup[i].seq == shotSeq) {
      return true;  // 重复
    }
  }
  _dedup[_dedupIdx].shooter = shooter;
  _dedup[_dedupIdx].seq = shotSeq;
  _dedupIdx = (_dedupIdx + 1) % 16;
  return false;
}

// 红外命中：带完整编码信息
bool GunState::applyHit(const LaserFrame &f, bool &killed) {
  killed = false;
  if (!_alive || _locked) return false;
  // 友军过滤：编码帧自带 team，优先用 team；无队伍(3)时退回 friends 列表
  if (f.team != 3 && f.team == (uint8_t)_team) return false;
  if (isFriend(f.playerId)) return false;
  // 精确去重：同一射击帧（同 playerId+shotSeq）只扣一次血
  if (hitDedup(f.playerId, f.shotSeq)) return false;
  doDamage(f.playerId, f.weaponId, killed);
  return true;
}

// 外部命中（头盔 X 帧转发，带 shotSeq）：按射手+时间窗去重（500ms）
bool GunState::applyExternalHit(uint16_t shooter, uint8_t weapon,
                                uint8_t shotSeq, bool &killed) {
  killed = false;
  if (!_alive || _locked) return false;
  if (isFriend(shooter)) return false;
  if (shooter == _extShooter && millis() - _extAt < 500) return false;
  _extShooter = shooter;
  _extAt = millis();
  doDamage(shooter, weapon, killed);
  (void)shotSeq;
  return true;
}

void GunState::doDamage(uint16_t shooter, uint8_t weapon, bool &killed) {
  killed = false;
  _lastShooter = shooter;
  _lastWeapon = weapon;
  _hp -= _dmg;
  _lastEvent = EV_HIT;
  if (_hp <= 0) {
    _hp = 0;
    _alive = false;
    _locked = true;                      // 阵亡锁扳机，等 R 帧
    _diedAt = millis();
    _lastEvent = EV_DEATH;
    killed = true;
  }
}

bool GunState::isFriend(uint16_t id) {
  if (id == _playerId) return true;  // 自己打自己不扣血
  for (uint8_t i = 0; i < _friendCount; i++)
    if (_friends[i] == id) return true;
  return false;
}
