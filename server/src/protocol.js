// 消息类型常量与基础校验（服务器侧协议见 docs/protocol.md）
export const MSG = {
  // 网关 → 服务器
  GATEWAY_HELLO: 'gatewayHello',
  DEV_HELLO: 'devHello',
  DEV_HIT: 'devHit',
  DEV_DEATH: 'devDeath',
  DEV_FIRE: 'devFire',
  DEV_HB: 'devHb',
  DEV_CAPTURE: 'devCapture',
  DEV_POS: 'devPos',
  DEV_TAG: 'devTag',
  DEV_LOG: 'devLog',
  DEV_PAIR: 'devPair',
  // 服务器 → 网关
  CMD: 'cmd',
  // 控制台 → 服务器
  CREATE: 'create',
  START: 'start',
  END: 'end',
  KICK: 'kick',
  STATS: 'stats',
  // 服务器 → 控制台
  STATE: 'state',
  EVENT: 'event',
};

export function isPlainObject(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

export function num(v, def = 0) {
  const n = Number(v);
  return Number.isFinite(n) ? n : def;
}

// 16 位序号窗口比较：返回 true 表示 seq 比 last 前进（含回绕）
export function seqAhead(seq, last) {
  if (last === undefined || last === null) return true;
  const diff = ((seq & 0xffff) - (last & 0xffff) + 0x10000) & 0xffff;
  return diff > 0 && diff < 0x8000;
}
