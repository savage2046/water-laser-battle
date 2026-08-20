# 无损改装水弹，激光模拟对战系统 (Water Laser Battle)
# 龟速项目 deepseek生成 未全系统验证 直接拿走 备注来源 谢谢

一套"水弹枪 + 激光模拟器"的完整对战系统：把普通水弹枪改装成激光对战装备，
支持**实时命中判定、伤害/血量系统、多模式对战（团队竞技 / 个人混战 / 夺旗）、
实时计分排行榜、470MHz LoRa 远距离联机（SX1262 SPI 直驱）、多网关自动漫游、
头盔接收器、GPS 战术地图、战绩持久化与对局回放**。

```
┌─────────────────┐   IR(38kHz)   ┌─────────────────┐
│  玩家 A 水弹枪    │ ◄──────────► │  玩家 B 水弹枪    │
│  ESP32+SX1262   │               │  ESP32+SX1262   │
└────────┬────────┘               └────────┬────────┘
         │ 470MHz LoRa（多网关共听/广播）    │
         ▼                                 ▼
┌───────────────────────┐   ┌───────────────────────┐
│  基地台网关 GW1         │   │  基地台网关 GW2         │   ← 多网关自动漫游
│  ESP32+SX1262+WiFi    │   │  ESP32+SX1262+WiFi    │
└──────────┬────────────┘   └──────────┬────────────┘
           └─────────────┬─────────────┘
                         ▼  WiFi/WebSocket
         ┌──────────────────────────────────────┐
         │  Node.js 对战服务器（中央裁判）         │
         │  对局管理 · 计分 · 去重 · 战绩 · 回放   │
         └──────────────────┬───────────────────┘
                            ▼
               Web 计分控制台 / 大屏排行榜
```

## 目录结构

```
water-laser-battle/
├── docs/                  # 架构、无线方案、TDMA MAC/协议、硬件接线、算力估算
├── firmware/
│   ├── lib/tdma/          # 共享 TDMA 库（TdmaMac 自适应 MAC + TdmaProto 10B 帧）
│   ├── gun/               # 水弹枪端固件（ESP32 + SX1262 470M + 双波段红外 + OLED）
│   ├── helmet/            # 头盔接收器固件（4路双波段红外 + 470M T帧/GPS上报）
│   ├── target/            # 激光校准靶（9路阵列 + 双波段解码 + OLED）
│   └── gateway/           # 基地台网关固件（470M⇄WebSocket 桥接 + 开机自检/多射频）
├── server/                # Node.js 对战服务器 + Web 控制台
└── tools/simulate.js      # 无硬件模拟器（模拟多网关 + 虚拟设备）
```

## 快速开始

### 1. 启动服务器（需要 Node.js ≥ 18）

```bash
cd server
npm install
npm start          # 默认 http://localhost:3000
```

浏览器打开 <http://localhost:3000> 即可使用计分控制台。

### 2. 无硬件先跑通（推荐）

```bash
cd server
node ../tools/simulate.js --count 6 --mode tdm --gateways 2 --score 20 --duration 60
```

模拟 2 个网关 + 6 台虚拟设备自动对战。`--gateways 2` 会故意让两个网关重复上报
同一事件，验证服务器按 `(deviceId, seq)` 去重（计分不重复）。

### 3. 真机固件

```bash
# 枪端：修改 firmware/gun/src/config.h（DEVICE_ID/DEV_IDX/引脚）
cd firmware/gun
pio run -t upload

# 网关：修改 firmware/gateway/src/config.h（GATEWAY_ID/WiFi/服务器地址）
cd firmware/gateway
pio run -t upload
```

需要 PlatformIO + ESP32 + SSD1306 OLED（I2C，枪端与网关各一块）。
接线见 [docs/hardware-design.md](docs/hardware-design.md)。

## 系统特性

| 功能 | 实现位置 |
| --- | --- |
| 激光命中判定（双波段同帧冗余 + 准直透镜远距增强） | firmware/gun: `LaserCodec` |
| 作用范围控制（功率档位 + 双帧 + 开火 <200ms） | firmware/gun: `LaserCodec` + server |
| 血量 / 弹药 / 装弹 / 重生状态机 | firmware/gun: `GunState` |
| 470MHz LoRa 上报（SX1262 SPI 直驱，紧凑帧 + seq，ALOHA 过渡版） | firmware: `RadioLink` |
| **TDMA 时隙 MAC（自适应 N=设备数，超帧 30+10N ms；开机自动分配信道/时隙）** | firmware/lib/tdma: `TdmaMac` |
| **10 字节定长二进制帧（CRC8，分片/重组）** | firmware/lib/tdma: `TdmaProto` |
| 无线升级方案：SX126x 多信道 TDMA（12 信道×17 时隙=200 设备 / 时延≤200ms / 1km） | docs/wireless-research.md |
| 无改装水弹枪联动（电流检测扳机 + 击杀/空弹物理断电 + ESP-NOW 音效同步，研究） | docs/trigger-sensor-study.md |
| 枪端显示屏（血量/弹药/状态/倒计时） | firmware/gun: `Display` |
| 头盔 GPS（NMEA 解析、位置归主人设备） | firmware/helmet: `Gps` |
| 音效播放（MAX98357A I2S，射击/命中/阵亡等） | firmware: `AudioPlayer` |
| 多色状态灯（WS2812，存活/阵亡/命中闪） | firmware: `LedStrip` |
| GPS 省电（电源开关 + 热启动，续航×10） | firmware/helmet: `gpsPowerUpdate` |
| 运动唤醒 GPS（LIS3DH 中断，静止 0mA 按需定位） | firmware/helmet: `MotionSensor` |
| 9 轴朝向记录（MPU9250，回放用） | firmware/helmet: `Imu9Axis` |
| 战术地图（Canvas 实时玩家位置） | server/public: `app.js` |
| 头盔接收器（4路双波段红外 + T帧上报 + master 转发） | firmware/helmet + server |
| 多网关漫游（共听上报、服务器去重、下行广播） | firmware/gateway + server |
| 网关间直连（UDP 组播，命中即时感知 <5ms） | firmware/gateway: `mcastSend/mcastPoll` |
| 网关显示屏（连接状态/设备数/最近命中事件） | firmware/gateway: `Display` |
| 对局创建/开始/结束、多模式规则 | server: `Match` + `modes.js` |
| 实时计分与排行榜 | server: `scoring.js` + Web 控制台 |
| 战绩持久化（JSON 文件） | server: `store.js` |
| 对局回放（JSONL 事件流） | server: `replay.js` |
| 激光校准靶（9路阵列 + 双波段解码 + OLED） | firmware/target |
| 户外强光自适应（环境光检测 + 自动升档） | firmware/gun + docs/outdoor-reliability.md |
| 随机 shotSeq 防作弊（硬件真随机） | firmware/gun: `esp_random()` |
| 一发一杀（网关唯一判定：跨波段 940 优先 + 同波段光强） | firmware/gateway + server |
| 设备日志（LogBuf 缓冲 + 时机上传 + 服务器落盘） | firmware/gun: `LogBuf` + server |
| 枪端菜单（3键项：配对头盔/加入战斗/校准激光） | firmware/gun: `Menu` |

## 通信协议

- 服务器侧 JSON over WebSocket：[docs/protocol.md](docs/protocol.md)
- 470MHz LoRa 紧凑帧（ALOHA 过渡版）：[docs/protocol-915m.md](docs/protocol-915m.md)
- **TDMA MAC 层（自适应时隙 + 开机自动分配信道）**：[docs/tdma-mac.md](docs/tdma-mac.md)
- **TDMA 10 字节定长帧协议**：[docs/protocol-tdma.md](docs/protocol-tdma.md)
- **无线升级方案研究（SX126x + 多信道 TDMA：200 设备 / 时延≤200ms / 1km / 10B 定长帧）**：[docs/wireless-research.md](docs/wireless-research.md)

## 文档

- [总体架构（含多网关漫游）](docs/architecture.md)
- [无线连接方案研究（SX126x 多信道 TDMA，初步决策）](docs/wireless-research.md)
- [ESP32 网关处理能力估算（CPU/RAM/SPI/GPIO）](docs/gateway-capacity.md)
- [无改装水弹枪联动方案研究（电流检测扳机 + 断电执行 + ESP-NOW）](docs/trigger-sensor-study.md)
- [检测板实现说明（trigger-board 固件 + 枪端集成）](docs/trigger-board.md)
- [硬件设计与接线](docs/hardware-design.md)
- [通信协议规范](docs/protocol.md)
- [470MHz LoRa 链路协议](docs/protocol-915m.md)
- [网关间直连通道](docs/gateway-mesh.md)
- [对战模式规则](docs/game-modes.md)
- [激光校准靶](docs/calibration-target.md)
- [户外强光可靠性](docs/outdoor-reliability.md)
- [项目状态与待办（暂停存档）](docs/roadmap.md)
- [成本估算](docs/cost-estimate.md)
