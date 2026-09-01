// 对战模式定义（规则见 docs/game-modes.md）
export const MODES = {
  tdm: {
    id: 'tdm',
    name: '团队竞技',
    teams: 2,
    respawnMs: 3000,
    scoreToWin: 50,
    friendlyFire: false,
    capture: false,
    powerLevel: 1,      // 0..3：0近 1标准 2远 3极限（两通道独立映射，见 game-modes.md）
  },
  ffa: {
    id: 'ffa',
    name: '个人混战',
    teams: 0,
    respawnMs: 3000,
    scoreToWin: 30,
    friendlyFire: false,
    capture: false,
    powerLevel: 1,
  },
  ctf: {
    id: 'ctf',
    name: '夺旗战',
    teams: 2,
    respawnMs: 5000,
    scoreToWin: 3,
    friendlyFire: false,
    capture: true,
    powerLevel: 1,
  },
};

export function getMode(id) {
  return MODES[id] || MODES.tdm;
}

export const DEFAULT_WEAPON = {
  hp: 100,
  dmg: 10,
  ammo: 120,
  reloadMs: 2000,
};

// 根据模式与玩家人数尽量均衡分队伍（0 队=无队伍）
export function balanceTeams(players, teamCount) {
  if (teamCount <= 0) return; // ffa：无队伍
  // 按当前队伍人数从少到多分配，使各队尽量均衡
  const counts = new Array(teamCount).fill(0);
  for (const p of players) {
    if (p.team >= 0 && p.team < teamCount) counts[p.team]++;
  }
  let minTeam = 0;
  for (let i = 1; i < teamCount; i++) if (counts[i] < counts[minTeam]) minTeam = i;
  // 只调整未分配(team=-1)的玩家
  for (const p of players) {
    if (p.team < 0) {
      p.team = minTeam;
      counts[minTeam]++;
      for (let i = 1; i < teamCount; i++) if (counts[i] < counts[minTeam]) minTeam = i;
    }
  }
}
