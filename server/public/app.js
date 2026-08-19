// Web 计分控制台前端
const MODE_NAMES = { tdm: '团队竞技', ffa: '个人混战', ctf: '夺旗战' };
const TEAM_NAMES = ['红队', '蓝队'];
const WEAPON_NAMES = ['步枪', '手枪', '狙击', '霰弹', '机枪'];

let state = { matches: [], players: [], gateways: [] };
let ws = null;
let lastEventTs = 0;

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
}[c]));

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => {
    $('conn-badge').textContent = '已连接';
    $('conn-badge').className = 'badge online';
    send({ t: 'stats' });
  };
  ws.onclose = () => {
    $('conn-badge').textContent = '未连接';
    $('conn-badge').className = 'badge offline';
    setTimeout(connect, 2000);
  };
  ws.onmessage = (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    handleMsg(msg);
  };
}

function send(obj) {
  if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
}

function handleMsg(msg) {
  switch (msg.t) {
    case 'state':
      state = msg;
      render();
      break;
    case 'event':
      pushEvent(msg);
      break;
    case 'stats':
      renderStats(msg.players || []);
      break;
  }
}

// ---------- 渲染 ----------
function render() {
  renderMatches();
  renderScoreboard();
  renderDevices();
  renderGateways();
  renderMap();
}

function renderMatches() {
  const box = $('match-list');
  box.innerHTML = '';
  for (const m of state.matches) {
    const el = document.createElement('div');
    el.className = 'item';
    const statusText = { waiting: '等待中', running: '进行中', finished: '已结束' }[m.status] || m.status;
    el.innerHTML = `
      <div class="name">${esc(m.name)} <span style="color:var(--dim)">[${MODE_NAMES[m.mode] || m.mode}]</span></div>
      <div class="meta">${m.id} · ${statusText} · 获胜 ${m.scoreToWin} 分 · ${m.scores.length} 人</div>
      <div class="actions">
        <button class="small primary" data-act="start" data-id="${m.id}" ${m.status !== 'waiting' ? 'disabled' : ''}>开始</button>
        <button class="small danger" data-act="end" data-id="${m.id}" ${m.status !== 'running' ? 'disabled' : ''}>结束</button>
      </div>`;
    box.appendChild(el);
  }
  box.querySelectorAll('button[data-act]').forEach((btn) => {
    btn.onclick = () => {
      const act = btn.dataset.act;
      const id = btn.dataset.id;
      send({ t: act, matchId: id });
    };
  });
}

function renderScoreboard() {
  const running = state.matches.find((m) => m.status === 'running')
    || state.matches.find((m) => m.status === 'waiting');
  $('match-title').textContent = running ? `— ${running.name}` : '';

  const teamsBox = $('team-scores');
  teamsBox.innerHTML = '';
  if (running && running.teams.length) {
    for (const t of running.teams) {
      teamsBox.insertAdjacentHTML('beforeend', `
        <div class="team-card t${t.id}">
          <div class="tname">${TEAM_NAMES[t.id] || '队伍' + t.id}</div>
          <div class="tscore">${t.score}</div>
        </div>`);
    }
  } else if (running) {
    teamsBox.innerHTML = '<div class="team-card"><div class="tname">个人混战</div></div>';
  }

  const tbody = $('scoreboard').querySelector('tbody');
  tbody.innerHTML = '';
  if (!running || !running.scores.length) {
    tbody.innerHTML = '<tr><td colspan="8" style="color:var(--dim)">暂无对局数据</td></tr>';
    return;
  }
  const sorted = [...running.scores].sort((a, b) => b.kills - a.kills || a.deaths - b.deaths);
  const players = new Map(state.players.map((p) => [p.playerId, p]));
  sorted.forEach((s, i) => {
    const p = players.get(s.playerId);
    const st = !p ? 'off' : (p.alive ? 'ok' : 'dead');
    const stText = { ok: '存活', dead: '阵亡', off: '离线' }[st];
    tbody.insertAdjacentHTML('beforeend', `
      <tr>
        <td>${i + 1}</td>
        <td><b>${esc(s.name)}</b></td>
        <td>${s.team >= 0 ? `<span class="team-chip t${s.team}">${TEAM_NAMES[s.team] || s.team}</span>` : '<span class="team-chip none">—</span>'}</td>
        <td style="color:var(--warn)">${s.kills}</td>
        <td>${s.deaths}</td>
        <td style="color:var(--ok)">${s.captures}</td>
        <td class="status-${st}">${stText}</td>
        <td><button class="small danger" data-kick="${s.playerId}">踢出</button></td>
      </tr>`);
  });
  tbody.querySelectorAll('button[data-kick]').forEach((btn) => {
    btn.onclick = () => send({ t: 'kick', playerId: Number(btn.dataset.kick) });
  });
}

function renderDevices() {
  $('dev-count').textContent = state.players.length;
  const box = $('dev-list');
  box.innerHTML = '';
  for (const p of state.players) {
    const m = state.matches.find((x) => x.id === p.matchId);
    box.insertAdjacentHTML('beforeend', `
      <div class="item">
        <div class="name"><span class="dot ${p.online ? 'on' : 'off'}"></span>${esc(p.name)}
          <span style="color:var(--dim)">#${p.playerId}</span></div>
        <div class="meta">${esc(p.deviceId)} · ${m ? esc(m.name) : '未入局'} · ${p.online ? '在线' : '离线'} · HP ${p.hp}</div>
      </div>`);
  }
  if (!state.players.length) box.innerHTML = '<div class="meta">暂无设备上线</div>';
}

function renderGateways() {
  $('gw-count').textContent = state.gateways.length;
  const box = $('gw-list');
  box.innerHTML = '';
  for (const g of state.gateways) {
    box.insertAdjacentHTML('beforeend', `
      <div class="item">
        <div class="name"><span class="dot ${g.online ? 'on' : 'off'}"></span>${esc(g.name)} <span style="color:var(--dim)">${esc(g.gatewayId)}</span></div>
        <div class="meta">${g.online ? '在线' : '离线'}</div>
      </div>`);
  }
  if (!state.gateways.length) box.innerHTML = '<div class="meta">暂无网关接入</div>';
}

function renderStats(players) {
  const box = $('stats-list');
  box.innerHTML = '';
  if (!players.length) {
    box.innerHTML = '<div class="meta">暂无战绩</div>';
    return;
  }
  for (const p of players.slice(0, 20)) {
    box.insertAdjacentHTML('beforeend', `
      <div class="item">
        <div class="name">${esc(p.name)} <span style="color:var(--dim)">#${p.playerId}</span></div>
        <div class="meta">${p.games} 局 · ${p.kills} 杀 · ${p.deaths} 死 · ${p.wins} 胜</div>
      </div>`);
  }
}

// ---------- 事件流 ----------
const FEED_MAX = 80;
function pushEvent(ev) {
  const feed = $('event-feed');
  const time = new Date(ev.ts || Date.now()).toLocaleTimeString('zh-CN', { hour12: false });
  let cls = 'item', text = '';
  switch (ev.kind) {
    case 'kill':
      cls = 'kill';
      text = `${ev.killerName} 🔫 击杀 ${ev.victimName}`;
      break;
    case 'capture':
      cls = 'capture';
      text = `${ev.name} 🚩 夺旗成功`;
      break;
    case 'matchStart':
      cls = 'match';
      text = `对局开始（${MODE_NAMES[ev.mode] || ev.mode}）`;
      break;
    case 'matchEnd':
      cls = 'match';
      text = `对局结束 · 胜者队伍 ${TEAM_NAMES[ev.winner] ?? `#${ev.winner}`}`;
      break;
    case 'respawn':
      cls = 'respawn';
      text = `${ev.name} 复活`;
      break;
    case 'hit':
      cls = '';
      text = `#${ev.shooter} 用${WEAPON_NAMES[ev.weapon] || '武器'}命中 #${ev.victim}（HP ${ev.hp}）`;
      break;
    default:
      return;
  }
  if (ev.ts && ev.ts < lastEventTs) return; // 忽略时间回退（防止旧事件插队）
  lastEventTs = ev.ts || Date.now();
  feed.insertAdjacentHTML('beforeend', `<div class="${cls}"><span class="ts">${time}</span>${text}</div>`);
  while (feed.children.length > FEED_MAX) feed.removeChild(feed.firstChild);
  feed.scrollTop = feed.scrollHeight;
}

// ---------- 战术地图（Canvas，GPS 玩家位置）----------
const TEAM_COLORS = ['#4f8cff', '#ff5c5c', '#3ddc84', '#ffb648'];

function renderMap() {
  const canvas = $('map-canvas');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;

  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = '#0b1120';
  ctx.fillRect(0, 0, W, H);

  // 网格
  ctx.strokeStyle = '#1c2740';
  ctx.lineWidth = 1;
  for (let x = 0; x <= W; x += 64) {
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
  }
  for (let y = 0; y <= H; y += 64) {
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
  }

  // 收集有定位的玩家
  const positioned = state.players.filter((p) => Number.isFinite(p.lat) && Number.isFinite(p.lon));
  if (!positioned.length) {
    ctx.fillStyle = '#55617a';
    ctx.font = '14px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('等待 GPS 定位…', W / 2, H / 2);
    ctx.textAlign = 'left';
    return;
  }

  // 计算经纬度范围（加 10% 边距）
  const lats = positioned.map((p) => p.lat);
  const lons = positioned.map((p) => p.lon);
  let minLat = Math.min(...lats), maxLat = Math.max(...lats);
  let minLon = Math.min(...lons), maxLon = Math.max(...lons);
  if (maxLat - minLat < 1e-4) { minLat -= 5e-5; maxLat += 5e-5; }
  if (maxLon - minLon < 1e-4) { minLon -= 5e-5; maxLon += 5e-5; }

  const pad = 40;
  const xOf = (lon) => pad + ((lon - minLon) / (maxLon - minLon)) * (W - pad * 2);
  const yOf = (lat) => pad + ((maxLat - lat) / (maxLat - minLat)) * (H - pad * 2);

  // 连线（示意：按加入顺序连接，形成路径感）
  ctx.strokeStyle = '#2c3a5e';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  positioned.forEach((p, i) => {
    const x = xOf(p.lon), y = yOf(p.lat);
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();

  // 玩家点 + 标签
  for (const p of positioned) {
    const x = xOf(p.lon), y = yOf(p.lat);
    const color = p.team >= 0 ? TEAM_COLORS[p.team % TEAM_COLORS.length] : '#8b98b3';
    ctx.beginPath();
    ctx.arc(x, y, 6, 0, Math.PI * 2);
    ctx.fillStyle = p.alive ? color : '#3a445c';
    ctx.fill();
    ctx.strokeStyle = '#e6ecf5';
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.font = '11px sans-serif';
    ctx.fillStyle = '#e6ecf5';
    ctx.textAlign = 'left';
    ctx.fillText(p.name, x + 9, y + 4);
  }
}

// ---------- 交互 ----------
$('create-form').onsubmit = (e) => {
  e.preventDefault();
  const name = $('match-name').value.trim() || '未命名对局';
  const mode = $('match-mode').value;
  const score = Number($('match-score').value);
  send({ t: 'create', name, mode, scoreToWin: Number.isFinite(score) && score > 0 ? score : undefined });
};
$('stats-btn').onclick = () => send({ t: 'stats' });

connect();
