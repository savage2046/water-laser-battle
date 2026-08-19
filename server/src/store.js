// 战绩持久化：JSON 文件（零外部依赖）
import fs from 'node:fs';
import path from 'node:path';

export class Store {
  constructor(file) {
    this.file = file;
    this.data = { players: {} };
    this._load();
  }

  _load() {
    try {
      if (fs.existsSync(this.file)) {
        const raw = fs.readFileSync(this.file, 'utf8');
        this.data = JSON.parse(raw) || { players: {} };
      }
    } catch (err) {
      console.error('[store] load failed:', err.message);
      this.data = { players: {} };
    }
  }

  _save() {
    try {
      fs.mkdirSync(path.dirname(this.file), { recursive: true });
      fs.writeFileSync(this.file, JSON.stringify(this.data, null, 2));
    } catch (err) {
      console.error('[store] save failed:', err.message);
    }
  }

  // 对局结束后累计战绩
  accumulateMatch(statsMap) {
    for (const s of Object.values(statsMap)) {
      const cur = (this.data.players[s.playerId] ??= {
        playerId: s.playerId,
        deviceId: s.deviceId,
        name: s.name,
        games: 0,
        kills: 0,
        deaths: 0,
        captures: 0,
        wins: 0,
      });
      cur.name = s.name || cur.name;
      cur.deviceId = s.deviceId || cur.deviceId;
      cur.games++;
      cur.kills += s.kills;
      cur.deaths += s.deaths;
      cur.captures += s.captures;
    }
    this._save();
  }

  addWin(playerId) {
    const p = this.data.players[playerId];
    if (p) {
      p.wins++;
      this._save();
    }
  }

  list() {
    return Object.values(this.data.players).sort(
      (a, b) => b.kills - a.kills || b.wins - a.wins
    );
  }
}
