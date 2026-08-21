# espnow-verify：电流检测板 ESP-NOW 信号验证固件

专用验证工具：接收 `firmware/trigger-board`（电流检测板）通过 ESP-NOW 上报的
**开火 / 心跳 / 异常**帧，串口实时显示、统计，并支持**下行断电/恢复测试**。
**不改动检测板固件**即可验证（默认自动伪装成枪端 MAC）。

## 帧协议（与 docs/trigger-board.md §4 一致）

| 方向 | type | d0 | 说明 |
| --- | --- | --- | --- |
| 板→本机 | 0x01 FIRE | 序号 | 每发开火脉冲 |
| 板→本机 | 0x02 HB | 计数 | 1Hz 心跳（3s 超时判离线） |
| 板→本机 | 0x03 FAULT | 0x01=堵转 | 异常（板已自动断电） |
| 本机→板 | 0x10 POWER | 0=断 1=通 | 下行测试（默认广播） |

## 硬件与烧录

任意 ESP32 / ESP32-S3（与检测板同信道 1，`ESPNOW_CHANNEL`）。

```bash
cd firmware/espnow-verify
pio run -t upload             # 默认 esp32s3（与检测板 PCB 一致）
pio run -e esp32dev -t upload # 经典 ESP32
pio device monitor            # 115200
```

接线：状态 LED 可接 GPIO2（`PIN_LED`）；不接也能用，串口即全部信息。

## 配对（三种方式，任选其一）

1. **默认（推荐，不改检测板）**：`SPOOF_GUN_MAC 1`，本机开机自动把 WiFi MAC
   临时改成检测板 `config.h` 里的 `GUN_MAC`，直接收到其单播帧；重启即还原。
2. 关闭伪装（`SPOOF_GUN_MAC 0`）：把本机开机打印的 `factory` MAC 填进检测板
   `config.h` 的 `GUN_MAC`，重刷检测板。
3. 若检测板固件改为广播发送，则无需任何配对。

> 注意：验证时不要同时开机其它 MAC 相同（=GUN_MAC）的设备。

## 串口命令

| 命令 | 作用 |
| --- | --- |
| `?` / `h` | 帮助 |
| `s` | 状态快照 |
| `r` | 清零统计 + 熄灭故障 LED |
| `v` | 切换详细模式（打印每帧原始数据） |
| `p0` / `p1` | 下行测试：发 FRAME_POWER 断电 / 恢复 |
| `p` | 切换（上次相反） |
| `m` | 打印本机/配对信息 |

## 输出格式（`#TAG` 前缀，便于脚本解析）

| 行 | 含义 |
| --- | --- |
| `#FIRE seq=.. rssi=.. rate1s=..` | 每发开火（seq 跳变先打 `#FIRE_GAP`） |
| `#HB n=.. interval=..ms rssi=..` | 每秒心跳；丢帧打 `#HB_GAP`，计数回退打 `#HB_RESET` |
| `#FAULT code=0x..` | 堵转等异常（LED 常亮，`r` 熄灭） |
| `#LINK online/offline` | 心跳 3s 超时判离线（变化时打一行） |
| `#STAT ...` | 每 2s 统计：计数/射速/丢包/RSSI/发送 ACK |
| `#SENDER n mac=.. frames=..` | 检测到的发送端（多板同测时区分） |
| `#TX FRAME_POWER=ON/OFF` | 下行指令已发（ACK 结果见 `#STAT` 的 tx_ok/tx_fail） |

## 验证判据

- [ ] 开机后 1~2s 内出现 `#HB`，随后每 ~1s 一条，`#STAT online=1`
- [ ] 扣扳机（单发/连发）→ 每发一条 `#FIRE`，seq 连续（无 `#FIRE_GAP`）
- [ ] 连发时 `rate1s` 与扳机射速吻合；`fire_min / fire_avg` 间隔合理
- [ ] `last_rssi` 稳定（一般 > -80dBm；为 0 表示旧内核回调不支持 RSSI，可忽略）
- [ ] 堵转测试：`#FAULT code=0x01` 出现，LED 常亮
- [ ] `p1` / `p0`：检测板 MOSFET 通/断生效（板端串口打印 `[pwr] ON/OFF`）
- [ ] 拔掉检测板电源 → 3s 内 `#LINK offline`；恢复 → `#LINK online`

## 常见问题

- **收不到任何帧**：先 `m` 看本机信道是否与检测板 `ESPNOW_CHANNEL` 一致；MAC
  伪装失败（`spoof=0`）时本机用 `factory` MAC，需按方式 2 改检测板 GUN_MAC。
- **RSSI 恒为 0**：PlatformIO espressif32 旧版本（Arduino core 2.x）的回调不带
  RSSI 字段，属正常；`platform = espressif32@^6` 即可获得 RSSI。
- **`#HB_GAP lost=N`**：偶发掉帧或检测板串口打印占用（不影响功能）。
- **多板同测**：各板 MAC 不同，用 `#SENDER` 区分；帧序号各自独立。
