# 通信协议规范（服务器侧，JSON over WebSocket）

服务器与**基地台网关群**、**Web 控制台**、**模拟器**之间统一使用 JSON over WebSocket。
网关负责把 470M LoRa 紧凑帧翻译为本文协议（见 [protocol-915m.md](protocol-915m.md)）。

**接入模型**：每条 WebSocket 连接 = 一个**网关**或一个**控制台**（首条消息类型区分）。
设备是服务器侧的**逻辑实体**，按 `deviceId` 唯一标识，与所在网关无关——
这是多网关漫游的基础。所有消息字段 `t` 表示类型，UTF-8 编码。

## 1. 消息总览

| 方向 | 类型 `t` | 用途 |
| --- | --- | --- |
| 网关→服务器 | `gatewayHello` | 网关注册 |
| 网关→服务器 | `devHello` | 设备注册（枪 J 帧） |
| 网关→服务器 | `devHit` / `devDeath` / `devFire` / `devHb` / `devCapture` / `devPos` | 设备事件（枪 H/D/F/B/C/P 帧） |
| 服务器→网关 | `cmd` | 设备指令（广播给所有网关） |
| 控制台→服务器 | `create` / `start` / `end` / `kick` / `stats` | 对局控制 |
| 服务器→控制台 | `state` | 全量快照 |
| 服务器→控制台 | `event` | 实时事件（击杀/夺旗/开始/结束） |

## 2. 网关 → 服务器

### gatewayHello（网关注册，连接建立后第一条）

```json
{ "t": "gatewayHello", "gatewayId": "GW1", "name": "基地台1", "fw": "1.0.0" }
```

### devHello（设备注册，转发枪的 J 帧）

```json
{ "t": "devHello", "gatewayId": "GW1", "deviceId": "G0001", "name": "玩家A", "fw": "1.0.0" }
{ "t": "devHello", "gatewayId": "GW1", "deviceId": "H0001", "name": "玩家A头盔", "master": "G0001", "fw": "1.0.0" }
```

重复 `devHello` 幂等：已注册设备直接返回当前 `welcome` 配置。
`master`：附属设备（头盔/背心接收器）声明其所属主设备（枪）的 `deviceId`；
主设备本身不携带该字段。服务器用 `master` 把附属设备的命中转发给主人的枪。

### devHit（设备被打中）

```json
{ "t": "devHit", "gatewayId": "GW1", "deviceId": "G0001", "seq": 42, "shooter": 7, "weapon": 2, "shotSeq": 180, "channel": 0, "hp": 30 }
```

### devDeath（设备阵亡）

```json
{ "t": "devDeath", "gatewayId": "GW1", "deviceId": "G0001", "seq": 43, "killer": 7, "shotSeq": 180, "channel": 0, "energy": 200, "newKill": true, "upgrade": false }
```

- `shotSeq`：红外帧解码的射击序号。
- `channel`：命中通道 **0=远距（38kHz）1=近距（56kHz）**（波长统一 940nm；远距击杀优先）。
- `energy`：光强度（0-255）——双帧窗口计数（200=强光、100=弱光）或 ADC 光强。
- `newKill`：**网关判定结果（唯一权威）**——同 `(killer, shotSeq)` 800ms 窗口
  内是否为当前最优击杀。
- `upgrade`：**抢占标记**——后到命中更优时服务器转移击杀归属（kills 数不变，
  victim 换为更优命中者）。
- **仲裁规则**：跨通道 **远距（38kHz）优先**（近距 56kHz 先到被计后远距可抢占）；
  **同通道（38k vs 38k、56k vs 56k）用光强度判定**，强光者优先。服务器不做去重判定，
  完全信任网关结果；多网关由组播协调。设备直连（无网关）时按首杀计。

### devFire / devHb / devCapture / devPos / devTag / devLog

```json
{ "t": "devFire", "gatewayId": "GW1", "deviceId": "G0001", "seq": 44 }
{ "t": "devHb",  "gatewayId": "GW1", "deviceId": "G0001", "seq": 45 }
{ "t": "devCapture", "gatewayId": "GW1", "deviceId": "G0001", "seq": 46 }
{ "t": "devPos", "gatewayId": "GW1", "deviceId": "H0001", "seq": 47, "lat": 31.234567, "lon": 121.456789, "yaw": 90, "pitch": 0, "roll": 0 }
{ "t": "devTag", "gatewayId": "GW1", "deviceId": "H0001", "seq": 48, "shooter": 7, "weapon": 2, "shotSeq": 180 }
{ "t": "devLog", "gatewayId": "GW1", "deviceId": "G0001", "seq": 49, "part": 0, "total": 2, "payload": "H,1,7,2;F,1" }
```

- `devCapture`：ctf 模式中玩家触碰旗点/夺旗按键触发。
- `devPos`：GPS 定位上报（度数）+ **头盔 9 轴朝向**（yaw/pitch/roll 整数度，
  磁北为 0 的航向）。移动超过阈值或每 30s 一次。**GPS 位于头盔（附属设备）**，
  服务器将位置归到主人的设备（玩家=枪），更新位置与方向并写入回放
  （kind `pos`，含朝向供回放复盘），控制台战术地图据此绘制。
- `devTag`：附属设备（头盔/背心接收器）上报被命中（`shooter` 为红外解码的
  射手 ID，`weapon` 为武器类型）。服务器按 `master` 关系找到主人的枪，
  向其下发 `{t:"control", action:"hit", shooter, weapon}`，由枪端本地扣血并
  上报（计分权威不变）。
- `devLog`：设备日志批量上传分片。`part` 从 0 起、`total` 总分片数；
  `payload` 为 `;` 分隔的日志条目。服务器按 `(deviceId, seq)` 去重后按
  `part` 拼接，齐全后写入 `data/logs/<deviceId>/<batch>.log`。

**去重规则**：服务器按 `deviceId` 维护 16 位 `seq` 窗口，仅处理前进的序号；
多网关重复上报的同一事件只处理一次。

## 3. 服务器 → 网关（广播给所有网关）

```json
{ "t": "cmd", "deviceId": "G0001", "msg": { "t": "welcome", "playerId": 3, "matchId": "m-xxxx", "cfg": { "mode": "tdm", "hp": 100, "dmg": 10, "ammo": 120, "reloadMs": 2000, "respawnMs": 3000, "team": 0, "friends": [3, 5, 9], "scoreToWin": 50, "powerLevel": 1 } } }
{ "t": "cmd", "deviceId": "G0001", "msg": { "t": "match", "status": "start", "mode": "tdm" } }
{ "t": "cmd", "deviceId": "G0001", "msg": { "t": "match", "status": "end", "winner": 0 } }
{ "t": "cmd", "deviceId": "G0001", "msg": { "t": "control", "action": "respawn" } }
{ "t": "cmd", "deviceId": "G0001", "msg": { "t": "control", "action": "pause" } }
{ "t": "cmd", "deviceId": "G0001", "msg": { "t": "control", "action": "resume" } }
{ "t": "cmd", "deviceId": "G0001", "msg": { "t": "control", "action": "hit", "shooter": 7, "weapon": 2, "shotSeq": 180 } }
{ "t": "cmd", "deviceId": "G0001", "msg": { "t": "control", "action": "vital", "playerId": 3, "hp": 75, "alive": 1 } }
```

- `control/hit`：由附属设备（头盔）命中触发，告知枪端"被射手 7 用武器 2 命中"，
  枪端本地扣血并照常上报 H/D（计分权威不变）。
- `control/vital`：生命状态同步——携带**玩家 ID**（头盔确认与枪端同一玩家）、
  血量、存活。开局/命中/重生/结束时下发给玩家的枪与头盔，使两个设备状态一致。

- `friends`：友军 ID 列表，用于枪端本地误伤过滤（红外帧不含队伍信息）。
- 广播给所有已注册网关；枪端重复接收无害（幂等）。

## 4. 控制台 → 服务器

```json
{ "t": "create", "name": "今晚大乱斗", "mode": "tdm", "scoreToWin": 50 }
{ "t": "start", "matchId": "m-xxxx" }
{ "t": "end", "matchId": "m-xxxx" }
{ "t": "kick", "playerId": 3 }
{ "t": "stats" }
```

## 5. 服务器 → 控制台

### state（全量快照，任何变化后广播）

```json
{
  "t": "state",
  "matches": [
    {
      "id": "m-xxxx", "name": "今晚大乱斗", "mode": "tdm",
      "status": "waiting", "scoreToWin": 50,
      "scores": [
        { "playerId": 3, "name": "玩家A", "kills": 5, "deaths": 2, "captures": 0, "team": 0 }
      ],
      "teams": [ { "id": 0, "score": 12 }, { "id": 1, "score": 8 } ]
    }
  ],
  "players": [
    { "playerId": 3, "name": "玩家A", "deviceId": "G0001", "matchId": "m-xxxx", "online": true, "hp": 100, "alive": true, "team": 0, "lat": 31.234567, "lon": 121.456789 }
  ],
  "gateways": [ { "gatewayId": "GW1", "name": "基地台1", "online": true } ]
}
```

### event（实时事件）

```json
{ "t": "event", "kind": "kill", "matchId": "m-xxxx", "killer": 3, "killerName": "玩家A", "victim": 7, "victimName": "玩家B", "ts": 1735000000000 }
{ "t": "event", "kind": "capture", "matchId": "m-xxxx", "playerId": 3, "name": "玩家A", "team": 0, "ts": 1735000000000 }
{ "t": "event", "kind": "matchStart", "matchId": "m-xxxx", "mode": "tdm", "ts": 1735000000000 }
{ "t": "event", "kind": "matchEnd", "matchId": "m-xxxx", "winner": 0, "ts": 1735000000000 }
{ "t": "event", "kind": "pos", "matchId": "m-xxxx", "playerId": 3, "name": "玩家A", "team": 0, "lat": 31.234567, "lon": 121.456789, "yaw": 90, "pitch": 0, "roll": 0, "ts": 1735000000000 }
```

`event` 同时写入该对局回放文件 `data/replays/<matchId>.jsonl`（每行一个事件）。

### stats（战绩查询响应）

```json
{ "t": "stats", "players": [ { "playerId": 3, "name": "玩家A", "games": 5, "kills": 20, "deaths": 8, "wins": 3 } ] }
```

## 6. 错误与健壮性

- 非法消息：服务器忽略并记录日志，不主动断开。
- 网关断线：其设备标记 `online:false`（保留对局战绩），网关重连后自动恢复。
- 设备心跳超时（>30s）：视为离线，不踢出对局。
- 设备在本机被击杀后，本地锁扳机直到收到 `respawn`。
- 设备漫游到新网关覆盖区无需任何动作：上行被新网关共听上报，
  下行由全网关广播。
