// 对局回放：JSONL 事件流，每行一个事件（与广播给控制台的 event 一致）
import fs from 'node:fs';
import path from 'node:path';

export class ReplayRecorder {
  constructor(matchId, dir) {
    this.matchId = matchId;
    this.dir = dir;
    this.fd = null;
  }

  open() {
    try {
      fs.mkdirSync(this.dir, { recursive: true });
      this.fd = fs.openSync(path.join(this.dir, `${this.matchId}.jsonl`), 'a');
    } catch (err) {
      console.error('[replay] open failed:', err.message);
    }
  }

  record(event) {
    if (!this.fd) return;
    try {
      fs.writeSync(this.fd, JSON.stringify(event) + '\n');
    } catch (err) {
      console.error('[replay] write failed:', err.message);
    }
  }

  close() {
    if (this.fd) {
      try {
        fs.closeSync(this.fd);
      } catch (_) { /* ignore */ }
      this.fd = null;
    }
  }
}
