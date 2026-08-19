// 计分逻辑：个人击杀/死亡/夺旗，队伍总分
export function computeScores(match) {
  const scores = [...match.players.values()]
    .map((p) => ({
      playerId: p.playerId,
      deviceId: p.deviceId,
      name: p.name,
      kills: p.kills,
      deaths: p.deaths,
      captures: p.captures,
      team: p.team,
    }))
    .sort((a, b) => b.kills - a.kills || a.deaths - b.deaths || a.playerId - b.playerId);
  return scores;
}

export function computeTeamScores(match) {
  const mode = match.mode;
  if (!mode.teams || mode.teams <= 0) return [];
  const teams = [];
  for (let i = 0; i < mode.teams; i++) {
    let score = 0;
    for (const p of match.players.values()) {
      if (p.team === i) score += p.kills;
    }
    if (mode.capture) {
      for (const p of match.players.values()) {
        if (p.team === i) score += p.captures;
      }
    }
    teams.push({ id: i, score });
  }
  return teams;
}

// 结算个人战绩（用于持久化累计）
export function finalizePlayerStats(match) {
  const stats = {};
  for (const p of match.players.values()) {
    stats[p.playerId] = {
      playerId: p.playerId,
      deviceId: p.deviceId,
      name: p.name,
      team: p.team,
      kills: p.kills,
      deaths: p.deaths,
      captures: p.captures,
    };
  }
  return stats;
}
