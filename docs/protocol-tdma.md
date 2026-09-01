# TDMA 10 字节定长帧协议（T1 实现）

> 状态：**已实现**（firmware/lib/tdma/TdmaProto）。替换 [protocol-915m.md](protocol-915m.md)
> 的 ASCII 紧凑帧（该文件为 ALOHA 过渡版，MAC 见 [tdma-mac.md](tdma-mac.md)）。
> 帧语义（J/H/D/F/B/C/P/T/L/A + W/S/E/R/X/V）与 `(deviceId, seq)` 去重规则不变。

## 1. 帧布局（固定 10 字节）

```
| devIdx(1B) | type+flags(1B) | seq(2B BE) | payload(5B) | crc8(1B) |
```

| 字段 | 说明 |
| --- | --- |
| devIdx | 8 位短设备号（0-255）；`0xFF`=广播/信标 |
| type | byte[1] 高 5 位（0-31） |
| flags | byte[1] 低 3 位 |
| seq | 16 位自增（上行去重；下行恒 0） |
| payload | 5 字节事件数据（各类型映射见下） |
| crc8 | CRC-8/ATM（poly 0x07），覆盖 byte[0..8] |

## 2. 帧类型与 payload 映射

### 上行（设备 → 网关，自身时隙发射）

| type | 名称 | payload[0..4] |
| --- | --- | --- |
| 0x01 JOIN | 注册 | deviceId 5 ASCII 字节（如 `G0005`）；flags bit0=头盔 |
| 0x02 HIT | 被打中 | shooter(2B BE) \| shotSeq \| weapon<<4\|channel \| hp |
| 0x03 DEATH | 阵亡 | killer(2B BE) \| shotSeq \| channel \| energy |
| 0x04 FIRE | 开火 | 全 0 |
| 0x05 HB | 心跳 | 全 0 |
| 0x06 CAPTURE | 夺旗 | 全 0 |
| 0x07 POS | 位置（3 片） | 片0: latE6(4B BE)；片1: lonE6(4B BE)；片2: yaw(2B BE)\|pitch\|roll |
| 0x08 TAG | 头盔命中 | shooter(2B BE) \| weapon \| shotSeq \| channel |
| 0x09 LOG | 日志分片 | part \| total \| data(3B) |
| 0x0A PAIR | 配对 | 全 0 |

POS 分片 flags：片0=`FIRST|MORE`，片1=`MORE`，片2=无。网关按 devIdx 重组 3 片后上报 devPos。

### 下行（网关 → 设备，广播窗发射）

| type | 名称 | payload[0..4] |
| --- | --- | --- |
| 0x00 BEACON | 信标（广播） | 超帧计数(4B BE) \| mapVer<<5\|N（**N 同时用作设备注册时的信道负载均衡依据**） |
| 0x0B WELCOME | 欢迎/规则（3 片） | 见下 |
| 0x0C START | 对局开始（广播） | mode（预留 0） |
| 0x0D END | 对局结束（广播） | winner（0xFF=平局） |
| 0x0E RESPAWN | 重生 | 全 0 |
| 0x0F PAUSE | 暂停（广播） | 全 0 |
| 0x10 RESUME | 恢复（广播） | 全 0 |
| 0x11 XHIT | 外部命中 | shooter(2B BE) \| weapon \| shotSeq \| channel |
| 0x12 VITAL | 生命同步 | playerId(2B BE) \| hp \| alive |
| 0x13 ASSIGN | 信道+时隙分配 | channelIdx \| slot \| N \| mapVer \| spare（slot=0xFF=信道满） |

WELCOME 3 片（flags：片0=`FIRST|MORE`，片1=`MORE`，片2=无）：
- 片0：playerId(2B BE) \| hp \| dmg \| ammo
- 片1：reloadMs(2B BE) \| respawnMs(2B BE) \| team(int8)
- 片2：scoreToWin \| powerLevel(0..3，两通道独立映射) \| friends devIdx 位图(24bit，T1 假定 playerId==devIdx)

## 3. 寻址与去重

- **下行寻址**：`devIdx` 字段即目标；非目标设备丢弃（广播 `0xFF` 全体收）。
  与 ASCII 版 `@<idx>:` 前缀语义一致。
- **上行去重**：`seq` 16 位自增，服务器按 `(deviceId, seq)` 去重（多网关共听
  冗余上报只处理一次）——规则不变，仅承载格式变化。
- **下行幂等**：无 seq，重复接收对设备状态机无害。

## 4. 网关翻译（470M 二进制 ⇄ 服务器 JSON）

| 二进制帧 | JSON 事件（t） |
| --- | --- |
| JOIN | `devHello`（+`helmet:true`，T1 无 master 字段） |
| HIT | `devHit` |
| DEATH | `devDeath`（含 channel/energy/newKill/upgrade，网关 killArbitrate 不变） |
| FIRE / HB / CAPTURE / PAIR | `devFire` / `devHb` / `devCapture` / `devPair` |
| POS（3 片重组） | `devPos` |
| TAG | `devTag` |
| LOG | `devLog` |
| 服务器 `cmd` | welcome→WELCOME×3；match/start|end→START|END（广播合并）；control/respawn|pause|resume|hit|vital→RESPAWN|PAUSE|RESUME|XHIT|VITAL |

## 5. 与旧 ASCII 帧的差异摘要

| 项 | ASCII（protocol-915m.md） | 10B 定长（本文件） |
| --- | --- | --- |
| 帧长 | ≤64B 变长 | 固定 10B |
| 设备标识 | 帧内文本 | devIdx 字节 |
| 校验 | 无 | CRC8（byte0-8） |
| 位置上报 | 单帧 30B+ | 3×10B 分片 |
| 日志 | 48B payload | 3B payload/片 |
| 名称/master | 文本字段 | T1 去除（T2 恢复） |
