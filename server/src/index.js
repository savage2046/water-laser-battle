// 水弹+激光模拟对战服务器入口
// - HTTP 静态服务（Web 控制台）
// - WebSocket /ws：网关接入（gatewayHello / dev*）、控制台接入（create/start/...）
// - 设备为逻辑实体（deviceId），多网关共听上报按 seq 去重，下行 cmd 广播所有网关
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { MSG, isPlainObject, num, seqAhead } from './protocol.js';
import { Match } from './game/Match.js';
import { getMode, MODES } from './game/modes.js';
import { Store } from './store.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PUBLIC_DIR = path.join(__dirname, '..', 'public');
const DATA_DIR = path.join(__dirname, '..', 'data');
const REPLAY_DIR = path.join(DATA_DIR, 'replays');
const PORT = process.env.PORT ? Number(process.env.PORT) : 3000;

const store = new Store(path.join(DATA_DIR, 'stats.json'));

// ---------- 注册表 ----------
const gateways = new Map(); // gatewayId -> { ws, name, online, lastSeen }
const consoles = new Set(); // ws 集合
const devices = new Map(); // deviceId -> device
const matches = new Map(); // matchId -> Match
let nextPlayerId = 1;

// ---------- 工具 ----------
function now() { return Date.now(); }

function send(ws, obj) {
  if (ws.readyState === 1) ws.send(JSON.stringify(obj));
}

// 广播设备指令给所有网关（多网关漫游：全部发，枪端幂等接收）
function sendCmd(deviceId, msg) {
  const out = JSON.stringify({ t: MSG.CMD, deviceId, msg });
  for (const g of gateways.values()) {
    if (g.online && g.ws.readyState === 1) g.ws.send(out);
  }
}

// 向玩家的枪 + 所有附属设备（头盔）同步生命状态，使两设备状态一致
// playerDevice：devices 中的设备对象（含 hp/alive/master）
function sendVital(playerDevice) {
  if (!playerDevice) return;
  const hp = playerDevice.hp ?? 100;
  const alive = playerDevice.alive ? 1 : 0;
  // 携带 playerId：头盔据此确认主人玩家 ID（与枪端同步）
  const vitalMsg = {
    t: 'control',
    action: 'vital',
    playerId: playerDevice.playerId ?? 0,
    hp,
    alive,
  };
  sendCmd(playerDevice.deviceId, vitalMsg);
  for (const [devId, dev] of devices) {
    if (dev.master === playerDevice.deviceId) {
      sendCmd(dev.deviceId, vitalMsg);
    }
  }
}

// 按 playerId 找到设备对象（Match 里的 player 只有 playerId/deviceId）
function sendVitalByDeviceId(deviceId) {
  const d = devices.get(deviceId);
  if (d) sendVital(d);
}

// 广播事件给所有控制台 + 写入回放
function broadcastEvent(ev) {
  const out = JSON.stringify(ev);
  for (const ws of consoles) {
    if (ws.readyState === 1) ws.send(out);
  }
  const m = matches.get(ev.matchId);
  if (m) m.replay.record(ev);
}

function broadcastState() {
  const state = {
    t: MSG.STATE,
    matches: [...matches.values()].map((m) => m.snapshot()),
    players: [...devices.values()].map((d) => ({
      playerId: d.playerId,
      deviceId: d.deviceId,
      name: d.name,
      matchId: d.matchId,
      online: d.online,
      hp: d.hp,
      alive: d.alive,
      team: d.team,
      lat: d.lat ?? null,
      lon: d.lon ?? null,
      yaw: d.yaw ?? null,
      pitch: d.pitch ?? null,
      roll: d.roll ?? null,
    })),
    gateways: [...gateways.values()].map((g) => ({
      gatewayId: g.gatewayId,
      name: g.name,
      online: g.online,
    })),
  };
  for (const ws of consoles) {
    if (ws.readyState === 1) ws.send(JSON.stringify(state));
  }
}

// ---------- 设备注册/事件 ----------
function ensureDevice(deviceId) {
  let d = devices.get(deviceId);
  if (!d) {
    d = {
      deviceId,
      name: deviceId,
      playerId: nextPlayerId++,
      matchId: null,
      master: null,             // 附属设备（头盔）所属主设备 deviceId
      online: false,
      lastSeen: 0,
      lastSeq: undefined,
      hp: 100,
      alive: true,
      team: -1,
    };
    devices.set(deviceId, d);
  }
  return d;
}

// 处理设备事件（多网关重复上报按 seq 去重）
function handleDevEvent(msg, type) {
  const d = devices.get(msg.deviceId);
  if (!d) return;
  const seq = num(msg.seq, -1);
  if (seq >= 0) {
    if (!seqAhead(seq, d.lastSeq)) return; // 重复/乱序，丢弃
    d.lastSeq = seq;
  }
  d.online = true;
  d.lastSeen = now();

  const m = d.matchId ? matches.get(d.matchId) : null;
  switch (type) {
    case MSG.DEV_HIT:
      if (m && m.status === 'running') {
        m.onHit({
          shooter: num(msg.shooter),
          victim: d.playerId,
          weapon: num(msg.weapon, 0),
          shotSeq: num(msg.shotSeq, 0),
          channel: num(msg.channel, 0),
          hp: num(msg.hp),
        });
        // 命中后同步枪+头盔生命状态（血量来自设备上报）
        d.hp = num(msg.hp, d.hp);
        sendVital(d);
      }
      break;
    case MSG.DEV_DEATH:
      if (m && m.status === 'running') {
        m.onDeath({
          killer: num(msg.killer),
          victim: d.playerId,
          shotSeq: num(msg.shotSeq, 0),
          channel: num(msg.channel, 0),
          energy: num(msg.energy, 128),
          newKill: typeof msg.newKill === 'boolean' ? msg.newKill : undefined,
          upgrade: msg.upgrade === true,
        });
        d.alive = false;
        sendVital(d);
      }
      break;
    case MSG.DEV_CAPTURE:
      if (m) m.onCapture({ playerId: d.playerId });
      break;
    case MSG.DEV_POS: {
      // GPS 位置 + 头盔 9 轴朝向：头盔（附属设备）上报归到主人设备（玩家=枪）
      if (typeof msg.lat === 'number' && typeof msg.lon === 'number' &&
          Number.isFinite(msg.lat) && Number.isFinite(msg.lon)) {
        const target = d.master ? devices.get(d.master) : d;
        if (target) {
          target.lat = msg.lat;
          target.lon = msg.lon;
          // 朝向（可选字段，整数度）
          if (typeof msg.yaw === 'number') target.yaw = msg.yaw;
          if (typeof msg.pitch === 'number') target.pitch = msg.pitch;
          if (typeof msg.roll === 'number') target.roll = msg.roll;
          const targetMatch = target.matchId ? matches.get(target.matchId) : null;
          if (targetMatch) {
            const ev = {
              t: 'event',
              kind: 'pos',
              matchId: targetMatch.id,
              playerId: target.playerId,
              name: target.name,
              team: target.team,
              lat: msg.lat,
              lon: msg.lon,
              yaw: target.yaw ?? 0,
              pitch: target.pitch ?? 0,
              roll: target.roll ?? 0,
              ts: now(),
            };
            targetMatch.replay.record(ev);
          }
        }
      }
      break;
    }
    case MSG.DEV_HB:
      break;
    case MSG.DEV_TAG: {
      // 头盔命中：按 master 关系找到主人的枪，下发 control/hit 由枪端本地扣血
      const masterId = d.master;
      if (masterId) {
        const gun = devices.get(masterId);
        if (gun) {
          sendCmd(gun.deviceId, {
            t: 'control',
            action: 'hit',
            shooter: num(msg.shooter),
            weapon: num(msg.weapon, 0),
            shotSeq: num(msg.shotSeq, 0),  // 随 H 上报供一发一杀去重
          });
        }
      }
      break;
    }
    default:
      break;
  }
}

// ---------- 设备日志（分片拼接 + 写文件）----------
const logBatches = new Map(); // deviceId -> { total, parts: [], lastAt }

function handleDevLog(msg) {
  const d = devices.get(msg.deviceId);
  if (!d) return;
  const seq = num(msg.seq, -1);
  if (seq >= 0) {
    if (!seqAhead(seq, d.lastSeq)) return; // 多网关重复上报，去重
    d.lastSeq = seq;
  }
  const part = num(msg.part, 0);
  const total = num(msg.total, 1);
  const payload = String(msg.payload || '');
  const key = d.deviceId;
  let b = logBatches.get(key);
  if (!b || b.total !== total) {
    b = { total, parts: [], lastAt: now() };
    logBatches.set(key, b);
  }
  b.parts[part] = payload;
  b.lastAt = now();
  // 分片齐全 → 写文件
  const filled = b.parts.filter((p) => p !== undefined).length;
  if (filled >= total) {
    const dir = path.join(DATA_DIR, 'logs', d.deviceId);
    try {
      fs.mkdirSync(dir, { recursive: true });
      const file = path.join(dir, `${now()}.log`);
      fs.writeFileSync(file, b.parts.join(''));
      console.log(`[log] ${d.deviceId} ${total} parts -> ${file}`);
    } catch (err) {
      console.error('[log] write failed:', err.message);
    }
    logBatches.delete(key);
  }
}

// ---------- 控制台命令 ----------
function handleConsoleMsg(ws, msg) {
  switch (msg.t) {
    case MSG.CREATE: {
      const mode = getMode(msg.mode);
      const m = new Match({
        name: String(msg.name || '').slice(0, 32),
        modeId: mode.id,
        scoreToWin: num(msg.scoreToWin, mode.scoreToWin),
        ctx: {
          sendCmd,
          broadcastEvent,
          store,
          replayDir: REPLAY_DIR,
          syncPlayerHp: (deviceId, hp, alive) => {
            const d = devices.get(deviceId);
            if (d) {
              d.hp = hp;
              d.alive = alive;
            }
          },
          sendVitalByDeviceId,
        },
      });
      m.replay.open();
      matches.set(m.id, m);
      // 已在等待中的主设备自动入队（附属设备如头盔不入局）
      for (const d of devices.values()) {
        if (!d.master && !d.matchId) {
          d.matchId = m.id;
          m.addPlayer(d);
        }
      }
      broadcastState();
      break;
    }
    case MSG.START: {
      const m = matches.get(msg.matchId);
      if (m) {
        m.start();
        broadcastState();
      }
      break;
    }
    case MSG.END: {
      const m = matches.get(msg.matchId);
      if (m) m.end(-1);
      broadcastState();
      break;
    }
    case MSG.KICK: {
      const d = [...devices.values()].find((x) => x.playerId === num(msg.playerId));
      if (d) {
        if (d.matchId) {
          const m = matches.get(d.matchId);
          if (m) m.removePlayer(d.playerId);
        }
        d.matchId = null;
        d.team = -1;
      }
      broadcastState();
      break;
    }
    case MSG.STATS: {
      send(ws, { t: MSG.STATS, players: store.list() });
      break;
    }
    default:
      break;
  }
}

// ---------- 连接分发 ----------
function handleMessage(ws, raw) {
  let msg;
  try {
    msg = JSON.parse(raw);
  } catch {
    console.warn('[ws] bad json');
    return;
  }
  if (!isPlainObject(msg)) return;

  // 网关连接（首条 gatewayHello）
  if (msg.t === MSG.GATEWAY_HELLO) {
    const gid = String(msg.gatewayId || '');
    if (!gid) return;
    const g = gateways.get(gid) || {};
    gateways.set(gid, {
      ws,
      gatewayId: gid,
      name: String(msg.name || gid),
      online: true,
      lastSeen: now(),
    });
    ws._gatewayId = gid;
    ws._role = 'gateway';
    console.log(`[gw] ${gid} online (${msg.name || ''})`);
    broadcastState();
    return;
  }

  // 网关上报设备事件
  if (ws._role === 'gateway') {
    const g = gateways.get(ws._gatewayId);
    if (g) g.lastSeen = now();
    switch (msg.t) {
      case MSG.DEV_HELLO: {
        const deviceId = String(msg.deviceId || '');
        if (!deviceId) return;
        const d = ensureDevice(deviceId);
        d.name = String(msg.name || deviceId).slice(0, 24);
        d.master = msg.master ? String(msg.master) : null; // 附属设备所属主设备
        d.online = true;
        d.lastSeen = now();
        // 仅在设备此前离线（首次注册/断线重连）时重置序号窗口；
        // 多网关重复上报 devHello 不重置，避免已处理事件被重复计分
        if (!d.wasOnline) d.lastSeq = undefined;
        d.wasOnline = true;
        // 自动加入一个 waiting 对局（附属设备如头盔不加入，仅主设备）
        if (!d.master && !d.matchId) {
          const waiting = [...matches.values()].find((m) => m.status === 'waiting');
          if (waiting) {
            d.matchId = waiting.id;
            waiting.addPlayer(d);
          }
        } else if (!d.master) {
          const m = matches.get(d.matchId);
          if (m && m.players.has(d.playerId)) m.resendWelcome(d); // 已在局中：只重发配置
        }
        console.log(`[dev] hello ${deviceId} (${d.name}) -> pid=${d.playerId}`);
        broadcastState();
        break;
      }
      case MSG.DEV_HIT:
      case MSG.DEV_DEATH:
      case MSG.DEV_HB:
      case MSG.DEV_FIRE:
      case MSG.DEV_CAPTURE:
      case MSG.DEV_POS:
      case MSG.DEV_TAG:
        handleDevEvent(msg, msg.t);
        break;
      case MSG.DEV_LOG:
        handleDevLog(msg);
        break;
      case MSG.DEV_PAIR: {
        // 配对请求：向本枪 + 其头盔同步生命状态（头盔据此确认玩家 ID）
        const d = devices.get(msg.deviceId);
        if (d) {
          sendVital(d);
          console.log(`[pair] ${d.deviceId} -> vital sync`);
        }
        break;
      }
      default:
        break;
    }
    broadcastState(); // 事件后刷新快照（节流可后续优化）
    return;
  }

  // 控制台连接
  if (ws._role === 'console') {
    handleConsoleMsg(ws, msg);
    return;
  }

  // 未知连接：首个消息决定角色
  if (msg.t === MSG.GATEWAY_HELLO) return handleMessage(ws, raw);
  ws._role = 'console';
  consoles.add(ws);
  handleConsoleMsg(ws, msg);
  send(ws, { t: MSG.STATE, ...currentState() });
}

function currentState() {
  return {
    matches: [...matches.values()].map((m) => m.snapshot()),
    players: [...devices.values()].map((d) => ({
      playerId: d.playerId,
      deviceId: d.deviceId,
      name: d.name,
      matchId: d.matchId,
      online: d.online,
      hp: d.hp,
      alive: d.alive,
      team: d.team,
      lat: d.lat ?? null,
      lon: d.lon ?? null,
      yaw: d.yaw ?? null,
      pitch: d.pitch ?? null,
      roll: d.roll ?? null,
    })),
    gateways: [...gateways.values()].map((g) => ({
      gatewayId: g.gatewayId,
      name: g.name,
      online: g.online,
    })),
  };
}

// ---------- 离线清扫 ----------
setInterval(() => {
  const cutoff = now() - 30_000;
  let changed = false;
  for (const [gid, g] of gateways) {
    if (g.online && g.lastSeen < cutoff) {
      g.online = false;
      console.log(`[gw] ${gid} offline`);
      changed = true;
    }
  }
  for (const d of devices.values()) {
    if (d.online && d.lastSeen < cutoff) {
      d.online = false;
      d.wasOnline = false; // 离线后重连视为新会话，重置序号窗口
      changed = true;
    }
  }
  // 日志批次超时（30s 未收全）清理，防内存泄漏
  for (const [k, b] of logBatches) {
    if (now() - b.lastAt > 30_000) logBatches.delete(k);
  }
  if (changed) broadcastState();
}, 15_000);

// ---------- HTTP 静态 + WS ----------
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

const server = http.createServer((req, res) => {
  let urlPath = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (urlPath === '/') urlPath = '/index.html';
  const file = path.join(PUBLIC_DIR, path.normalize(urlPath));
  if (!file.startsWith(PUBLIC_DIR)) {
    res.writeHead(403);
    res.end('forbidden');
    return;
  }
  fs.readFile(file, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end('not found');
      return;
    }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
});

const wss = new WebSocketServer({ server, path: '/ws' });
wss.on('connection', (ws) => {
  console.log('[ws] connection');
  ws.on('message', (data) => {
    handleMessage(ws, data.toString('utf8'));
  });
  ws.on('close', () => {
    if (ws._role === 'console') consoles.delete(ws);
    if (ws._role === 'gateway' && ws._gatewayId) {
      const g = gateways.get(ws._gatewayId);
      if (g && g.ws === ws) g.online = false;
      console.log(`[gw] ${ws._gatewayId} disconnected`);
      broadcastState();
    }
  });
  ws.on('error', () => {});
});

server.listen(PORT, () => {
  console.log(`[server] http://localhost:${PORT}  (控制台)`);
  console.log(`[server] ws://localhost:${PORT}/ws  (网关/模拟器)`);
  console.log(`[server] 支持模式: ${Object.keys(MODES).join(', ')}`);
});
