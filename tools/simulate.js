#!/usr/bin/env node
// 无硬件模拟器：用与网关相同的 WS 协议模拟多个网关 + 若干虚拟设备。
// 多网关模式（--gateways >= 2）会故意让多个网关重复上报同一事件，
// 验证服务器按 (deviceId, seq) 去重。
//
// 用法：
//   node tools/simulate.js --count 6 --mode tdm --gateways 2 --score 20 --duration 60
import WebSocket from 'ws';

const args = process.argv.slice(2);
function opt(name, def) {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : def;
}
function flag(name) {
  return args.includes(`--${name}`);
}

const COUNT = Number(opt('count', 6));
const GATEWAYS = Math.max(1, Number(opt('gateways', 1)));
const MODE = opt('mode', 'tdm');
const SCORE = Number(opt('score', ''));
const DURATION = Number(opt('duration', 120));
const URL = opt('url', 'ws://localhost:3000/ws');

console.log(`[sim] 网关数=${GATEWAYS} 设备数=${COUNT} 模式=${MODE} 时长=${DURATION}s`);
console.log(`[sim] 连接 ${URL}`);

// 每个设备一个 Bot：本地维护 hp/alive，随机互射；带模拟 GPS 位置（上海附近）
class Bot {
  constructor(deviceId, name) {
    this.deviceId = deviceId;
    this.name = name;
    this.playerId = 0;
    this.team = -1;
    this.hp = 100;
    this.maxHp = 100;
    this.dmg = 10;
    this.alive = true;
    this.matchId = null;
    this.inMatch = false;
    this.seq = 0;
    this.nextFire = Date.now() + Math.random() * 3000;
    // 模拟 GPS：基准点 31.2304, 121.4737（上海人民广场），偏移 ±0.01°
    this.lat = 31.2304 + (Math.random() - 0.5) * 0.02;
    this.lon = 121.4737 + (Math.random() - 0.5) * 0.02;
    this.nextPos = Date.now() + Math.random() * 3000;
    this._lastPosLat = this.lat;
    this._lastPosLon = this.lon;
  }
  nextSeq() {
    this.seq = (this.seq + 1) & 0xffff;
    return this.seq;
  }
  // 随机游走（每步 ~0.001°，约 100m）
  walk() {
    this.lat += (Math.random() - 0.5) * 0.002;
    this.lon += (Math.random() - 0.5) * 0.002;
    // 限制在基准点附近，避免漂远
    this.lat = Math.max(31.21, Math.min(31.25, this.lat));
    this.lon = Math.max(121.45, Math.min(121.50, this.lon));
  }
  posMoved() {
    const dLat = Math.abs(this.lat - this._lastPosLat) * 111320;
    const dLon = Math.abs(this.lon - this._lastPosLon) * 111320 * Math.cos(this.lat * Math.PI / 180);
    return Math.sqrt(dLat * dLat + dLon * dLon) >= 8;
  }
  commitPos() {
    this._lastPosLat = this.lat;
    this._lastPosLon = this.lon;
  }
}

// 虚拟网关：一个 WS 连接，负责 N 台设备的上行/下行
class VirtualGateway {
  constructor(id, bots) {
    this.id = id;
    this.bots = bots;
    this.ws = null;
    this.connected = false;
  }

  connect() {
    this.ws = new WebSocket(URL);
    this.ws.on('open', () => {
      this.connected = true;
      console.log(`[gw ${this.id}] connected`);
      this.ws.send(JSON.stringify({ t: 'gatewayHello', gatewayId: this.id, name: `虚拟网关${this.id.slice(2)}` }));
    });
    this.ws.on('message', (raw) => this.onMessage(String(raw)));
    this.ws.on('close', () => {
      this.connected = false;
      console.log(`[gw ${this.id}] disconnected, reconnect in 3s`);
      setTimeout(() => this.connect(), 3000);
    });
    this.ws.on('error', () => {});
  }

  send(obj) {
    if (this.connected && this.ws.readyState === 1) this.ws.send(JSON.stringify(obj));
  }

  onMessage(raw) {
    let msg;
    try { msg = JSON.parse(raw); } catch { return; }
    if (msg.t !== 'cmd') return;
    const bot = this.bots.find((b) => b.deviceId === msg.deviceId);
    if (!bot) return;
    const m = msg.msg;
    switch (m.t) {
      case 'welcome':
        bot.playerId = m.playerId;
        bot.matchId = m.matchId;
        bot.maxHp = m.cfg.hp;
        bot.dmg = m.cfg.dmg;
        bot.hp = m.cfg.hp;
        bot.team = m.cfg.team;
        bot.inMatch = true;
        console.log(`[bot ${bot.name}] welcome pid=${bot.playerId} team=${bot.team}`);
        break;
      case 'match':
        if (m.status === 'start') {
          bot.alive = true;
          bot.hp = bot.maxHp;
          console.log(`[bot ${bot.name}] match start (${m.mode})`);
        } else if (m.status === 'end') {
          bot.alive = false;
          bot.inMatch = false;
          console.log(`[bot ${bot.name}] match end winner=${m.winner}`);
        }
        break;
      case 'control':
        if (m.action === 'respawn') {
          bot.alive = true;
          bot.hp = bot.maxHp;
          console.log(`[bot ${bot.name}] respawn`);
        } else if (m.action === 'hit') {
          // 头盔命中转发：本地扣血
          if (bot.alive && bot.inMatch) {
            bot.hp = Math.max(0, bot.hp - bot.dmg);
            if (bot.hp <= 0) {
              bot.alive = false;
              console.log(`[bot ${bot.name}] helmet-killed by P${m.shooter}`);
            }
          }
        }
        break;
    }
  }

  // 设备心跳
  heartbeat(bot) {
    this.send({ t: 'devHb', gatewayId: this.id, deviceId: bot.deviceId, seq: bot.nextSeq() });
  }
}

// ---------- 主流程 ----------
let startedMatchId = null;

const bots = [];
for (let i = 0; i < COUNT; i++) {
  bots.push(new Bot(`G${String(i + 1).padStart(4, '0')}`, `玩家${String.fromCharCode(65 + (i % 26))}${i + 1}`));
}

const gateways = [];
for (let g = 0; g < GATEWAYS; g++) {
  const gwBots = bots; // 所有网关都能听到所有设备
  gateways.push(new VirtualGateway(`GW${g + 1}`, gwBots));
}
for (const gw of gateways) gw.connect();

// 等待网关连接后注册设备（每把枪配一个头盔附属设备）
setTimeout(() => {
  for (const gw of gateways) {
    for (const bot of gw.bots) {
      gw.send({ t: 'devHello', gatewayId: gw.id, deviceId: bot.deviceId, name: bot.name, fw: 'sim-1.0.0' });
      // 头盔：master = 所属枪 deviceId
      gw.send({
        t: 'devHello', gatewayId: gw.id, deviceId: `H${bot.deviceId.slice(1)}`,
        name: `${bot.name}头盔`, master: bot.deviceId, fw: 'sim-1.0.0',
      });
    }
  }
  console.log(`[sim] 已注册 ${COUNT} 台设备 + ${COUNT} 个头盔`);

  // 用控制台连接创建并开始对局（监听广播的 state 找到 waiting 对局）
  const ws = new WebSocket(URL);
  ws.on('open', () => {
    const create = { t: 'create', name: `模拟对局-${Date.now().toString(36)}`, mode: MODE };
    if (SCORE > 0) create.scoreToWin = SCORE;
    ws.send(JSON.stringify(create));
    // 服务器广播 state 时自动开始 waiting 对局
    ws.on('message', (raw) => {
      let msg;
      try { msg = JSON.parse(String(raw)); } catch { return; }
      if (msg.t === 'state') {
        const waiting = msg.matches.find((m) => m.status === 'waiting');
        if (waiting && !startedMatchId) {
          startedMatchId = waiting.id;
          ws.send(JSON.stringify({ t: 'start', matchId: waiting.id }));
          console.log(`[sim] 对局 ${waiting.id} 开始`);
        }
      }
    });
  });
}, 1500);

// 心跳
setInterval(() => {
  for (const gw of gateways) {
    for (const bot of gw.bots) {
      if (Math.random() < 0.3) gw.heartbeat(bot);
    }
  }
}, 5000);

// 模拟头盔命中：随机上报 devTag（由服务器转发给主人的枪扣血）
let helmetSeq = 0;
setInterval(() => {
  const aliveBots = bots.filter((b) => b.inMatch && b.alive);
  if (aliveBots.length < 2) return;
  const victim = aliveBots[Math.floor(Math.random() * aliveBots.length)];
  const attackers = aliveBots.filter((b) => b !== victim);
  const attacker = attackers[Math.floor(Math.random() * attackers.length)];
  const gw = gateways[0];
  gw.send({
    t: 'devTag', gatewayId: gw.id, deviceId: `H${victim.deviceId.slice(1)}`,
    seq: (++helmetSeq) & 0xffff, shooter: attacker.playerId,
    weapon: Math.floor(Math.random() * 5),
    shotSeq: Math.floor(Math.random() * 256),
    channel: Math.random() < 0.7 ? 0 : 1,
  });
}, 2500);

// 模拟 GPS 位置（头盔上报）：随机游走 + 上报（移动超阈值或 5s 定时）
// 服务器会把头盔位置归到主人设备（玩家=枪），战术地图据此绘制
setInterval(() => {
  for (const gw of gateways) {
    for (const bot of gw.bots) {
      if (!bot.inMatch) continue;
      bot.walk();
      const moved = bot.posMoved();
      const now = Date.now();
      if (moved || now - (bot._lastPosAt || 0) >= 5000) {
        bot._lastPosAt = now;
        bot.commitPos();
        // 模拟头盔 9 轴朝向（yaw 随位置游走变化）
        bot.yaw = (bot.yaw || 0) + Math.floor(Math.random() * 60 - 30);
        bot.yaw = ((bot.yaw % 360) + 360) % 360;
        gw.send({
          t: 'devPos', gatewayId: gw.id, deviceId: `H${bot.deviceId.slice(1)}`,
          seq: bot.nextSeq(), lat: Number(bot.lat.toFixed(6)), lon: Number(bot.lon.toFixed(6)),
          yaw: bot.yaw, pitch: 0, roll: 0,
        });
      }
    }
  }
}, 3000);

// 模拟对战：随机互射
setInterval(() => {
  const aliveBots = bots.filter((b) => b.inMatch && b.alive);
  if (aliveBots.length < 2) return;
  const attacker = aliveBots[Math.floor(Math.random() * aliveBots.length)];
  const victims = aliveBots.filter((b) => b !== attacker);
  const victim = victims[Math.floor(Math.random() * victims.length)];

  // 命中（由被打者上报，带武器/序号/通道）
  victim.hp -= attacker.dmg;
  const hpLeft = Math.max(0, victim.hp);
  const weapon = Math.floor(Math.random() * 5);  // 0-4 武器
  const shotSeq = Math.floor(Math.random() * 256);  // 0-255 模拟红外帧序号
  const channel = Math.random() < 0.7 ? 0 : 1;  // 0=远距 38k 1=近距 56k（远距为主，波长统一 940nm）
  const gw = gateways[0];
  gw.send({
    t: 'devHit', gatewayId: gw.id, deviceId: victim.deviceId,
    seq: victim.nextSeq(), shooter: attacker.playerId, weapon, shotSeq, channel, hp: hpLeft,
  });
  // 多网关重复上报同一事件
  if (GATEWAYS > 1) {
    for (let g = 1; g < GATEWAYS; g++) {
      if (gateways[g].connected) {
        gateways[g].send({
          t: 'devHit', gatewayId: gateways[g].id, deviceId: victim.deviceId,
          seq: victim.seq, shooter: attacker.playerId, weapon, shotSeq, channel, hp: hpLeft,
        });
      }
    }
  }

  if (victim.hp <= 0) {
    victim.alive = false;
    victim.hp = 0;
    // 模拟网关判定：首杀 newKill=true；energy 模拟同通道光强（双帧强弱）
    const energy = Math.random() < 0.5 ? 200 : 100;
    const gw2 = gateways[0];
    gw2.send({
      t: 'devDeath', gatewayId: gw2.id, deviceId: victim.deviceId,
      seq: victim.nextSeq(), killer: attacker.playerId, shotSeq, channel,
      energy, newKill: true,
    });
    if (GATEWAYS > 1) {
      for (let g = 1; g < GATEWAYS; g++) {
        if (gateways[g].connected) {
          gateways[g].send({
            t: 'devDeath', gatewayId: gateways[g].id, deviceId: victim.deviceId,
            seq: victim.seq, killer: attacker.playerId, shotSeq, channel,
            energy, newKill: true,
          });
        }
      }
    }
    console.log(`[sim] ${attacker.name} 击杀 ${victim.name}`);
  }
}, 1500);

// 限时退出
setTimeout(() => {
  console.log(`[sim] ${DURATION}s 到，退出`);
  for (const gw of gateways) if (gw.ws) gw.ws.close();
  process.exit(0);
}, DURATION * 1000);
