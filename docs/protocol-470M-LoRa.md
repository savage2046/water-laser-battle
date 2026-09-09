# 470MHz LoRa 链路协议（枪 ⇄ 网关群，SX1262 SPI 直驱）

> **⚠️ MAC 层升级预告**：本文件为 **ALOHA 过渡版**协议。无线链路已确定升级为
> **多信道 TDMA**（SX126x，SF7/500k × 12 信道 × 17 时隙 = 204 设备，时延 ≤200ms，
> 1km，10 字节定长二进制帧）——方案见 [wireless-research.md](wireless-research.md)。
> TDMA 落地时，本文件的 ASCII 帧将重设计为二进制定长帧（roadmap 待办 T1/T2），
> 帧语义（J/H/D/F/B/C/P/T/L/A + W/S/E/R/X/V）与 `(deviceId, seq)` 去重规则保持不变。

LoRa 带宽有限（470.0MHz、125kHz、SF8 时约 3.1kbps 有效载荷），因此使用
**紧凑 ASCII 帧**而非 JSON。每帧为一个 LoRa 包（≤ 64 字节），一包一帧
（LoRa 自带长度，无需 `\n` 分隔符）。基地台网关负责与服务器侧 JSON 协议互译。

**多网关漫游**：所有网关同频共听。上行帧会被覆盖范围内**所有**网关收到并各自上报，
服务器按 `(deviceId, seq)` 去重；下行帧由**所有**网关同时发射，枪必然收到。
设备无需选择或切换网关。

## 1. 上行帧（枪 → 网关群）

**设备标识**：SX1262 为广播信道，网关无法从物理层区分发送者，因此每帧必须携带
**8 位短设备号 `devIdx`**（0~255，每把枪在固件 `config.h` 中配置唯一值）。
网关从 `J` 帧学习 `devIdx → deviceId` 映射，事件帧用 `devIdx` 查表恢复
`deviceId`；查不到时丢弃（枪每 5s 重发 J 直到收到 welcome，映射必然建立）。

| 帧 | 格式 | 示例 | 说明 |
| --- | --- | --- | --- |
| 注册 | `J<devIdx>,<devId>,<name>` | `J5,G0005,玩家E` | 开机/掉线重连时发送；无 seq |
| 被打中 | `H<devIdx>,<seq>,<shooter>,<weapon>,<shotSeq>,<channel>,<hp>` | `H5,42,7,2,180,0,30` | `shooter`=射手 ID；`weapon`=武器；`shotSeq`=射击序号（一发一杀去重）；`channel`=命中通道 **0=远距 38kHz 1=近距 56kHz**（波长统一 940nm，远距击杀优先）；`hp`=剩余血量 |
| 阵亡 | `D<devIdx>,<seq>,<killer>,<shotSeq>,<channel>,<energy>` | `D5,43,7,180,0,200` | 本地血量归零时发送；`channel`=命中通道（0=远距 38k 1=近距 56k）；`energy`=光强度（双帧窗口 200=强 100=弱）。仲裁：跨通道远距（38kHz）优先，**同通道光强高者优先** |
| 开火 | `F<devIdx>,<seq>` | `F5,44` | 开火统计（可选） |
| 心跳 | `B<devIdx>,<seq>` | `B5,45` | 每 10s；同时用于网关/服务器判断设备在线 |
| 夺旗 | `C<devIdx>,<seq>` | `C5,46` | ctf 模式触碰旗点/夺旗按键触发 |
| 位置+方向 | `P<devIdx>,<seq>,<latE6>,<lonE6>,<yaw>,<pitch>,<roll>` | `P5,47,31234567,121456789,90,0,0` | GPS 定位 + 头盔 9 轴朝向（yaw 0-359 磁北为0，pitch -90..90，roll -180..180，整数度）；移动超过阈值或每 30s 上报 |
| 头盔命中 | `T<devIdx>,<seq>,<shooter>,<weapon>,<shotSeq>,<channel>` | `T9,48,7,2,180,0` | 头盔接收器被射手 7 命中（含序号与通道） |
| 日志分片 | `L<devIdx>,<seq>,<part>,<total>,<payload>` | `L5,50,0,2,H,1,7,2;F,1` | 设备日志上传分片：`part` 从 0 起，`total` 总分片数，`payload` 为 `;` 分隔的日志条目 |
| 配对请求 | `A<devIdx>,<seq>` | `A5,51` | 枪端"连接头盔"菜单触发：服务器向本枪 + 其头盔同步玩家 ID（V 帧） |

- `seq`：设备侧 16 位自增序号，每个事件帧递增一次，用于服务器去重
  （多网关重复上报只处理一次）。回绕后按 16 位窗口比较仍安全。

## 2. 下行帧（网关群 → 枪，全网关广播 + 目标寻址）

**寻址**：470M 为广播介质，所有枪都能听到所有下行帧。每帧以 `@<devIdx>:` 前缀
标明目标设备（`devIdx` 为设备 8 位短号），非目标设备直接丢弃；网关从 `devHello`
学习的映射中解析目标 `devIdx`。**头盔（附属设备）也参与下行寻址**——接收
对局状态同步（S/E/V 帧）以与主人枪端状态一致。

| 帧 | 格式 | 示例 | 说明 |
| --- | --- | --- | --- |
| 欢迎/规则 | `@<idx>:W<playerId>,<mode>,<hp>,<dmg>,<ammo>,<reloadMs>,<respawnMs>,<team>,<friends>,<scoreToWin>[,<powerLevel>]` | `@3:W3,tdm,100,10,120,2000,3000,0,3;5;9,50,1` | `friends` 用 `;` 分隔的友军 ID 列表；**无友军（ffa）时用 `-` 占位**；`team`=-1 表示无队伍；`powerLevel`=激光作用范围档位（**0..3**：0近 1标准 2远 3极限；两通道独立映射，近距通道（56kHz/940nm）由 2 位 GPIO（IR_PWR_850_* 历史命名）选 4 档电流，见 hardware-design §6.5；可选，默认 1） |
| 重生 | `@<idx>:R` | `@3:R` | 阵亡玩家复活（幂等） |
| 对局开始 | `@<idx>:S<mode>` | `@3:Stdm` | 复位血量弹药、解锁扳机 |
| 对局结束 | `@<idx>:E<winner>` | `@3:E0` | 锁扳机等待下一局；`winner`=-1 平局 |
| 暂停 | `@<idx>:P` | `@3:P` | 锁扳机（幂等） |
| 恢复 | `@<idx>:Q` | `@3:Q` | 解锁扳机（幂等） |
| 外部命中 | `@<idx>:X<shooter>,<weapon>,<shotSeq>,<channel>` | `@3:X7,2,180,0` | 服务器转发的头盔命中：本枪被射手 7 命中（本地扣血）；含通道供远距（38kHz）优先 |
| 生命同步 | `@<idx>:V<playerId>,<hp>,<alive>` | `@3:V3,75,1` | 主人玩家 ID + 生命状态同步（发给头盔/枪）：开局/命中/重生/结束时下发，两设备据此确认同一玩家 |

下行帧无 seq：重复接收对枪状态机幂等，无需去重。

## 3. 示例会话（双网关）

```
枪 ──J 5,G0005,玩家E ───────────────► GW1/GW2 ──devHello──► 服务器 (首个生效)
头盔 ──J 9,H0001,玩家A头盔,G0001 ───► GW1/GW2 ──devHello(master)──► 服务器
枪 ◄──@5:W3,tdm,100,10,120,2000,3000,0,3;5;9,50 ◄──── 服务器 cmd ──► GW1/GW2 同时发射
枪 ◄──@5:S tdm ◄────────────────────────────────────── 服务器 cmd
枪 ──H 5,42,7,2,30 ──► GW1 ──devHit──► 服务器 (处理，weapon=2 狙击)
枪 ──H 5,42,7,2,30 ──► GW2 ──devHit──► 服务器 (seq 42 ≤ 已处理 42，丢弃)
枪 ──D 5,43,7 ────► GW1/GW2 ──devDeath──► 服务器 (只处理一次)
头盔 ──T 9,48,7,0 ──► 网关 ──devTag──► 服务器 (master=G0005 → 找主人的枪)
枪 ◄──@5:X7,0 ◄───────────────────────────────────────── 服务器 cmd(control/hit)
枪 ◄──@5:R ◄────────────────────────────────────────── 服务器 cmd(respawn)
```

## 4. 网关翻译规则

上行（470M → JSON，均带 `gatewayId` 与 `deviceId`）：
- `J` → `{t:"devHello", gatewayId, deviceId, name, fw}`；若帧含第 4 字段
  （`J<devIdx>,<devId>,<name>,<master>`）则附加 `master`（附属设备如头盔）
- `H` → `{t:"devHit", gatewayId, deviceId, seq, shooter, weapon, shotSeq, hp}`
- `D` → `{t:"devDeath", gatewayId, deviceId, seq, killer, shotSeq}`
- `F` → `{t:"devFire", gatewayId, deviceId, seq}`
- `B` → `{t:"devHb", gatewayId, deviceId, seq}`
- `C` → `{t:"devCapture", gatewayId, deviceId, seq}`
- `P` → `{t:"devPos", gatewayId, deviceId, seq, lat, lon, yaw, pitch, roll}`（lat/lon 由微度转回度数；yaw/pitch/roll 为头盔朝向整数度）
- `T` → `{t:"devTag", gatewayId, deviceId, seq, shooter, weapon, shotSeq}`（头盔被命中）
- `L` → `{t:"devLog", gatewayId, deviceId, seq, part, total, payload}`（日志分片）
- `A` → `{t:"devPair", gatewayId, deviceId, seq}`（配对：同步玩家 ID 到头盔）

下行（JSON → 470M）：服务器 `{t:"cmd", deviceId, msg}` → 按 `msg.t` 翻译：
- `welcome` → `W` 帧（按字段顺序拼接）
- `match/start` → `S<mode>`；`match/end` → `E<winner>`
- `control/respawn` → `R`；`pause` → `P`；`resume` → `Q`；`hit` → `X<shooter>,<weapon>,<shotSeq>`；
  `vital` → `V<playerId>,<hp>,<alive>`

## 5. 健壮性

- 上行无 ACK（原型简化）；服务器按 seq 去重，多网关冗余上报天然容错。
- 枪端收不到 `W` 时每 5s 重发 `J`；服务器对重复 `devHello` 幂等处理。
- 多枪同时上行可能碰撞，帧短（≤64B）且事件稀疏，碰撞概率低；
  网关共听进一步降低丢帧影响（一个网关丢帧，另一个可能收到）。
- 网关断线重连后重新 `gatewayHello` 注册，服务器网关列表自动恢复。
