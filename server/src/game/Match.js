// 单局对战：生命周期、计分、模式规则、重生调度、事件广播
import { getMode, balanceTeams, DEFAULT_WEAPON } from './modes.js';
import {
  computeScores,
  computeTeamScores,
  finalizePlayerStats,
} from './scoring.js';
import { ReplayRecorder } from './replay.js';

let matchSeq = 0;

export class Match {
  constructor({ name, modeId, scoreToWin, ctx }) {
    this.id = `m-${++matchSeq}-${Date.now().toString(36)}`;
    this.name = name || '未命名对局';
    this.mode = getMode(modeId);
    this.scoreToWin = scoreToWin || this.mode.scoreToWin;
    this.status = 'waiting'; // waiting | running | finished
    this.winner = -1;
    this.players = new Map(); // playerId -> player
    this.ctx = ctx; // { sendCmd, broadcastEvent, store, replayDir }
    this.replay = new ReplayRecorder(this.id, ctx.replayDir);
    this._respawnTimers = new Map();
    this._killSeen = new Map(); // 击杀归属记录：(killer:shotSeq) -> {ts, victim}（供 upgrade 转移，非去重）
  }

  get weapon() {
    return { ...DEFAULT_WEAPON, dmg: this.mode.dmg || DEFAULT_WEAPON.dmg };
  }

  snapshot() {
    return {
      id: this.id,
      name: this.name,
      mode: this.mode.id,
      status: this.status,
      scoreToWin: this.scoreToWin,
      winner: this.winner,
      scores: computeScores(this),
      teams: computeTeamScores(this),
    };
  }

  addPlayer(player) {
    if (this.status === 'finished') return false;
    player.team = -1; // 未分配，由 balanceTeams 分配
    player.kills = 0;
    player.deaths = 0;
    player.captures = 0;
    player.alive = true;
    player.hp = this.weapon.hp;
    this.players.set(player.playerId, player);
    balanceTeams([...this.players.values()], this.mode.teams);
    this._sendWelcome(player);
    return true;
  }

  removePlayer(playerId) {
    const p = this.players.get(playerId);
    this.players.delete(playerId);
    this._clearRespawn(playerId);
    return p;
  }

  // 设备已在对局中：仅重发当前配置（断线重连/多网关重复上报时调用，不清零战绩）
  resendWelcome(player) {
    this._sendWelcome(player);
  }

  start() {
    if (this.status !== 'waiting') return false;
    this.status = 'running';
    for (const p of this.players.values()) {
      p.alive = true;
      p.hp = this.weapon.hp;
      this._sendWelcome(p); // 下发最新规则（含队伍/友军）
      this._sendMatch(p, 'start');
      // 开局：同步设备镜像 + 枪/头盔生命状态（两设备一起进入对局）
      if (this.ctx.syncPlayerHp) this.ctx.syncPlayerHp(p.deviceId, p.hp, p.alive);
      if (this.ctx.sendVitalByDeviceId) this.ctx.sendVitalByDeviceId(p.deviceId);
    }
    const ev = {
      t: 'event',
      kind: 'matchStart',
      matchId: this.id,
      mode: this.mode.id,
      ts: Date.now(),
    };
    this._broadcast(ev);
    return true;
  }

  end(winner = -1) {
    if (this.status === 'finished') return;
    this.status = 'finished';
    this.winner = winner;
    for (const p of this.players.values()) {
      this._sendMatch(p, 'end', winner);
      this._clearRespawn(p.playerId);
    }
    const ev = {
      t: 'event',
      kind: 'matchEnd',
      matchId: this.id,
      winner,
      scores: computeScores(this),
      teams: computeTeamScores(this),
      ts: Date.now(),
    };
    this._broadcast(ev);

    // 战绩累计 + 胜场
    const stats = finalizePlayerStats(this);
    this.ctx.store.accumulateMatch(stats);
    if (winner >= 0) {
      for (const p of this.players.values()) {
        // 团队模式：胜者队伍全体记胜；个人模式：胜者玩家本人记胜
        const isWinner = this.mode.teams > 0 ? p.team === winner : p.playerId === winner;
        if (isWinner) this.ctx.store.addWin(p.playerId);
      }
    }
    this.replay.close();
  }

  // 设备被打中（服务器镜像 + 命中事件，不直接计分）
  onHit({ shooter, victim, weapon = 0, hp }) {
    if (this.status !== 'running') return;
    const v = this.players.get(victim);
    if (!v) return;
    v.hp = hp;
    // 命中事件供控制台展示（可选）
    const ev = {
      t: 'event',
      kind: 'hit',
      matchId: this.id,
      shooter,
      victim,
      weapon,
      hp,
      ts: Date.now(),
    };
    this._broadcast(ev);
  }

  // 设备阵亡 → 计分 + 胜局判定 + 调度重生
  // 判定完全由网关完成（newKill/upgrade，组播协调多网关），服务器仅执行结果：
  //   newKill=true  → 计击杀（kills+1）
  //   upgrade=true  → 抢占（940 优先/同波段光强），击杀归属转移（kills 数不变，
  //                   victim 换更优命中者）
  //   newKill=false → 网关判定同发重复，不计击杀（deaths 照常 +1）
  // _killSeen 仅记录当前击杀归属（供 upgrade 转移），不做去重判定。
  onDeath({ killer, victim, shotSeq, channel = 0, newKill, upgrade }) {
    if (this.status !== 'running') return;
    const v = this.players.get(victim);
    const k = this.players.get(killer);
    if (!v) return;
    v.alive = false;
    v.deaths++;
    if (k && k !== v) {
      // 友军误伤防护（本地已过滤，服务器再兜底）
      if (!this.mode.friendlyFire && k.team === v.team && this.mode.teams > 0) {
        // 不计分
      } else {
        const key = `${killer}:${shotSeq ?? 0}`;
        const now = Date.now();
        // 940 抢占：归属转移给 940 命中者（kills 已计过，不重复 +1）
        if (upgrade) {
          const prev = this._killSeen.get(key);
          if (prev && now - prev.ts <= 800) {
            this._killSeen.set(key, { ts: now, victim });
            this._broadcast({
              t: 'event', kind: 'kill', matchId: this.id,
              killer, killerName: k.name, victim, victimName: v.name,
              team: v.team, ts: now,
            });
          }
        } else if (newKill === true) {
          // 网关判定首杀：计击杀（去重已由网关保证）
          k.kills++;
          this._killSeen.set(key, { ts: now, victim });
          if (this._killSeen.size > 4096) this._killSeen.clear();
          this._broadcast({
            t: 'event', kind: 'kill', matchId: this.id,
            killer, killerName: k.name, victim, victimName: v.name,
            team: v.team, ts: now,
          });
        }
        // newKill === false：网关判定同发重复，忽略（deaths 已 +1）
        // newKill === undefined：设备未过网关（直连/旧网关），信任并计首杀
        else {
          k.kills++;
          this._killSeen.set(key, { ts: now, victim });
          if (this._killSeen.size > 4096) this._killSeen.clear();
          this._broadcast({
            t: 'event', kind: 'kill', matchId: this.id,
            killer, killerName: k.name, victim, victimName: v.name,
            team: v.team, ts: now,
          });
        }
      }
    }
    this._scheduleRespawn(victim);
    this._checkWin();
  }

  // 夺旗（ctf 模式，硬件按键触发 devCapture）
  onCapture({ playerId }) {
    if (this.status !== 'running') return;
    if (!this.mode.capture) return;
    const p = this.players.get(playerId);
    if (!p) return;
    p.captures++;
    const ev = {
      t: 'event',
      kind: 'capture',
      matchId: this.id,
      playerId,
      name: p.name,
      team: p.team,
      ts: Date.now(),
    };
    this._broadcast(ev);
    this._checkWin();
  }

  _scheduleRespawn(playerId) {
    this._clearRespawn(playerId);
    const timer = setTimeout(() => {
      this._respawnTimers.delete(playerId);
      if (this.status !== 'running') return;
      const p = this.players.get(playerId);
      if (!p) return;
      p.alive = true;
      p.hp = this.weapon.hp;
      this.ctx.sendCmd(p.deviceId, { t: 'control', action: 'respawn' });
      // 重生后同步枪+头盔生命状态（ctx.syncPlayerHp 由服务器提供，同步设备镜像）
      if (this.ctx.syncPlayerHp) this.ctx.syncPlayerHp(p.deviceId, p.hp, p.alive);
      if (this.ctx.sendVitalByDeviceId) this.ctx.sendVitalByDeviceId(p.deviceId);
      const ev = {
        t: 'event',
        kind: 'respawn',
        matchId: this.id,
        playerId,
        name: p.name,
        ts: Date.now(),
      };
      this._broadcast(ev);
    }, this.mode.respawnMs);
    this._respawnTimers.set(playerId, timer);
  }

  _clearRespawn(playerId) {
    const t = this._respawnTimers.get(playerId);
    if (t) {
      clearTimeout(t);
      this._respawnTimers.delete(playerId);
    }
  }

  _checkWin() {
    if (this.status !== 'running') return;
    if (this.mode.teams > 0) {
      for (const team of computeTeamScores(this)) {
        if (team.score >= this.scoreToWin) {
          this.end(team.id);
          return;
        }
      }
    } else {
      for (const s of computeScores(this)) {
        if (s.kills >= this.scoreToWin) {
          this.end(s.playerId);
          return;
        }
      }
    }
  }

  _sendWelcome(player) {
    const cfg = {
      mode: this.mode.id,
      hp: this.weapon.hp,
      dmg: this.weapon.dmg,
      ammo: this.weapon.ammo,
      reloadMs: this.weapon.reloadMs,
      respawnMs: this.mode.respawnMs,
      team: player.team,
      friends:
        this.mode.teams > 0
          ? [...this.players.values()]
              .filter((p) => p.team === player.team && p.playerId !== player.playerId)
              .map((p) => p.playerId)
          : [],
      scoreToWin: this.scoreToWin,
      powerLevel: this.mode.powerLevel ?? 1,  // 激光作用范围档位 0..3（两通道独立映射）
    };
    this.ctx.sendCmd(player.deviceId, {
      t: 'welcome',
      playerId: player.playerId,
      matchId: this.id,
      cfg,
    });
  }

  _sendMatch(player, status, winner = -1) {
    const msg = { t: 'match', status, mode: this.mode.id };
    if (status === 'end') msg.winner = winner;
    this.ctx.sendCmd(player.deviceId, msg);
  }

  _broadcast(ev) {
    this.ctx.broadcastEvent(ev); // 控制台 + 回放
  }
}
