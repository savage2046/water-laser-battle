# 项目状态与待办清单（暂停存档）

> 最后更新：无线方案研究 + T1 TDMA 实现 + **无改装水弹枪联动方案研究/固件原型**
> （电流检测扳机 + 击杀/空弹物理断电 + ESP-NOW）
> 返回接续时先读本文件（§0 会话存档）+ [README.md](README.md) + [architecture.md](docs/architecture.md)

## 0. 会话存档：无线方案研究 + T1 TDMA 实现（本轮）

> 目的：后续接续无需重读全部过程，本节省略决策与文件清单。

### 0.1 结论摘要（已定案）
- **无线技术**：SX126x（SX1262/SX1268）470MHz LoRa，模组 E22-400M22S/Ra-01SH 级
- **MAC**：多信道 TDMA（FDMA+TDMA）——单信道 LoRa 数学上无法容纳 200 设备×200ms
- **目标参数**：SF7/BW500k/CR4:5/**前导 4**（10B 帧空口 9.28ms）；标准频点栅格
  **470.0 + k×2.0MHz（20 点）**；自适应超帧 **30+10N ms**（N=信道活跃设备数 1..17，
  N=17 → 200ms）；**开机自动分配信道/时隙 + 注册负载均衡**；10B 定长二进制帧（CRC8）；
  TX **15dBm 传导（17dBm e.r.p. 合规，原 22dBm 超限）**
- **容量**：200 设备 = 12 信道 × 17 时隙；单跳时延 ≤200ms（设备少时更低）；1km ✓
  （150dB 链路预算，15dBm 即够）
- **合规**：工信部 2019 年 52 号公告，470-510MHz 微功率 e.r.p. ≤50mW（17dBm）

### 0.2 本轮实现清单（代码状态：**已写完，未编译/未烧录实测**）
| 文件 | 内容 |
| --- | --- |
| `firmware/lib/tdma/TdmaProto.h/.cpp`（新） | 10B 定长帧（19 类型 + CRC8 + 分片）+ 标准栅格常量 |
| `firmware/lib/tdma/TdmaMac.h/.cpp`（新） | 自适应 TDMA MAC（FreeRTOS 任务 core1 优先级 5；信标/下行窗/时隙/注册槽；负载均衡扫描；网关重排 mapVer；任务栈可配 `TDMA_TASK_STACK_WORDS`） |
| `firmware/gun/src/` | main.cpp 接入 TDMA（二进制事件帧 + W 三分片重组 + F 帧 1s 合并）；config.h 射频参数 + 标准栅格；RadioLink.h 加 getRadio |
| `firmware/helmet/src/` | 同枪端（JOIN 带 TF_FLAG_HELMET；P 帧 3 片）；修复 GPS 状态机先使用后声明 |
| `firmware/gateway/src/` | main.cpp 开机自检（槽位探测→质量检测→频率分配→每射频 MAC）；RadioLink 多实例化；config.h 槽位数/质量阈值 |
| 三端 `platformio.ini` | `lib_extra_dirs = ../lib` |

### 0.3 文档（全部已更新）
| 文档 | 说明 |
| --- | --- |
| [docs/wireless-research.md](docs/wireless-research.md) | 方案研究（候选对比/SX126x 选型/容量数学/合规/结论） |
| [docs/tdma-mac.md](docs/tdma-mac.md) | MAC 规范（超帧/信标/注册/重排/负载均衡/自检/质量检测/限制） |
| [docs/protocol-tdma.md](docs/protocol-tdma.md) | 10B 帧协议（类型/payload/分片/翻译） |
| [docs/gateway-capacity.md](docs/gateway-capacity.md) | ESP32 网关算力估算（T1 够用；T3 瓶颈 RAM/GPIO） |
| [docs/protocol-915m.md](docs/protocol-915m.md) | 标注为 ALOHA 过渡版（帧语义保留） |
| [docs/architecture.md](docs/architecture.md) / [README.md](README.md) | 决策表 + 链接同步 |

### 0.4 未验证事项（下一步优先）
1. **编译**：本会话终端环境故障（pwsh 0xC0000142）无法 `pio run` —— **接续第一步先编译三端固件**，修复可能的编译错误
2. **烧录实测**：注册收敛时间、时延随 N 变化（示波器/日志打点）、1km 丢包率（前导 4/6/8 三档）、多设备并发开机注册碰撞、网关信道质量检测阈值校准（-95/-85dBm 为经验值）
3. **任务栈实测**：`uxTaskGetStackHighWaterMark` 校准 `TDMA_TASK_STACK_WORDS`（单射频 4096 字假设）
4. **时隙命中率**：多射频错峰后 ≥99% 目标；RX 盲区（连续 RX 只在窗口首武装）已处理，需实测确认

### 0.5 下一步路线（roadmap §4 已挂账）
- T3b：多信道网关硬件（12×SX1262 板，槽位表 kRfSlots 填实际引脚；**主控建议 ESP32-S3**，RAM/GPIO 是真瓶颈）
- T3c：多网关信标仲裁（主/备切换 + 漫游）
- T2 后续：W 帧友军位图映射修正（playerId≠devIdx）、JOIN 携带设备名/master、深睡省电
- 遗留坑：设备扫描全栅格 20 点 ≈ 5.25s（一次性；快速测试可临时 `TDMA_CHANNELS 1`）

### 0.6 会话存档补充：无改装水弹枪联动（第二轮，代码已实现待实测）

> 目标：不改装水弹枪，电池处串联检测板——电流波形识别扳机/连发（逐发音效同步），
> 击杀/空弹物理断电防作弊；检测板 ↔ 枪端主控用 ESP-NOW（2.4G）。

**结论摘要**：水弹电机电流特征显著（阶跃+脉冲串，连发 10-20Hz）→ 电流检测可行；
ESP-NOW 单跳 2-5ms（端到端 5-15ms）满足音效同步；物理断电是唯一硬防作弊手段；
470M TDMA 系统零改动复用。

**实现清单（代码已写完，未编译/未实测）**
| 文件 | 内容 |
| --- | --- |
| `firmware/trigger-board/`（新工程） | 检测板：INA226/ACS712 采样 + 识别状态机（扳机/连发/堵转）+ MOSFET 断电 + ESP-NOW（fire/hb/fault/power 4 字节帧） |
| `firmware/gun/src/GunEspNow.h/.cpp`（新） | 枪端 ESP-NOW：开火事件源 + 断电/恢复指令 + 心跳监控 |
| `firmware/gun/src/main.cpp` | 扳机双源（微动 || ESP-NOW）；`gunPowerUpdate()` 断电（条件：存活&&有弹&&未锁） |
| `firmware/gun/src/config.h` | `GUN_ESPNOW_ENABLE`/`TRIGGER_BOARD_MAC`/`ESPNOW_CHANNEL` |

**关键参数（config.h，实测校准）**
- 电流阈值 `TRIG_CURRENT_A 1.0`、去抖 10ms、连发最小间隔 20ms、堵转 2s、结束 50ms
- 断电条件：`isAlive && ammo>0 && !isLocked`（阵亡/空弹/暂停/结束/未开局 → 断电）
- 配对：检测板 `GUN_MAC` 与枪端 `TRIGGER_BOARD_MAC` **互填对方真实 MAC**

**待办（后续接续顺序）**
1. 编译验证三端（gun / gateway / trigger-board）——会话终端故障未跑 `pio run`
2. 多枪型电流波形采集 → 阈值校准（识别准确率 ≥99%）
3. 硬件打样：INA226 + 2mΩ 分流 + 逻辑电平 MOSFET（RDS<2mΩ）+ ESP32-C3
4. 端到端实测：ESP-NOW 延时 P95、断电响应 <50ms、同场多枪互扰、断电重上电副作用

**文档**：[docs/trigger-sensor-study.md](docs/trigger-sensor-study.md)（研究）｜
[docs/trigger-board.md](docs/trigger-board.md)（实现：接线/状态机/帧协议/集成点）

## 1. 当前完成的功能

| 模块 | 状态 |
| --- | --- |
| 470MHz LoRa 链路（SX1262 SPI 直驱，RadioLib） | ✅ 枪/盔/网关三端 |
| **TDMA MAC（自适应超帧 30+10N ms / 信标 / 注册时隙 / 动态重排）** | ✅ 代码就绪（待烧录实测） |
| **开机自动分配信道/时隙 + 注册负载均衡（扫全栅格选 N 最小）** | ✅ 代码就绪 |
| **网关开机自检（槽位探测 + 信道质量检测 + 频率自动分配 + 没装满兼容）** | ✅ 代码就绪 |
| **10 字节定长二进制帧（19 类型 + CRC8 + 分片/重组）** | ✅ 代码就绪 |
| **无改装联动检测板（电流传感 + MOSFET 断电 + ESP-NOW；枪端 GunEspNow 集成）** | ✅ 代码就绪（待硬件实测） |
| 多网关漫游（共听上报 + (deviceId,seq) 去重 + 下行广播 + @idx 寻址） | ✅ |
| 网关间 UDP 组播（命中即时感知 <5ms + 仲裁同步） | ✅ |
| 命中/血量/弹药/重生（服务器权威计分） | ✅ |
| 随机 shotSeq 防作弊（esp_random） | ✅ |
| 一发一杀（网关唯一判定：跨波段 940 优先 + 同波段光强度判定） | ✅ |
| 头盔：4 路双波段接收 + GPS（省电：电源开关+运动唤醒）+ 9 轴朝向 + 音效灯效 | ✅ |
| 枪端：OLED 菜单（配对/加入/校准）+ 音效 + WS2812 灯 | ✅ |
| 服务器：对局/计分/战绩/回放/日志落盘/战术地图 | ✅ |
| Web 控制台（实时计分板 + Canvas 战术地图 + 事件流） | ✅ |
| 校准靶（9 路阵列 + 双波段解码） | ✅ |
| 模拟器（多网关 + 虚拟设备 + 头盔 + GPS + 朝向） | ✅ |
| 成本估算 | ✅ docs/cost-estimate.md |

## 2. 近几轮成果记录（本次会话完整演进）

### 硬件/传感
- **GPS 省电**：电源开关（AO3401 控制 VCC）+ 热启动（V_BCKP 保 RTC）+ **LIS3DH 运动唤醒**（静止 GPS 0mA，运动中断立即定位）
- **9 轴 IMU（MPU9250）**：记录头盔朝向（pitch/roll 加速度计、yaw 磁力计），随 P 帧上报，回放用；按需上电省电
- **音效 + 多色灯**：头盔去 OLED → WS2812 灯 + MAX98357A 喇叭；枪端加喇叭灯；音效程序合成（射击/命中/阵亡/重生/开局/结束）
- **引脚让位**：枪端夺旗键 GPIO14 → I2S BCLK；SX_DIO1 GPIO4 → I2S WS；蜂鸣器 33 → I2S DOUT（ctf 夺旗键移除）

### 激光链路
- **双波段同帧冗余**：完整 40bit 帧（playerId+weapon+team+shotSeq+chk）双通道并行，任一通道解码成功即命中
- **850nm 限距 ≤20m**：固定低功率（独立限流 R3，不随 powerLevel 变档），20m 外刻意不触发；20m+ 由 940nm 透镜通道承担
- **双帧发射**（同 shotSeq 2 帧）+ 准直透镜（940nm）提升远距可靠性

### 防作弊与击杀仲裁（最终定型）
- **一发一杀**：同 (killer, shotSeq) 800ms 窗口只计 1 击杀，防光束扫到多人刷分
- **网关唯一判定权威**：`killArbitrate`（首杀直发 / 抢占 upgrade / 重复不转发），服务器仅执行结果（kills+1 / 归属转移），无本地去重判定
- **仲裁规则**：跨波段 **940 优先**（850 先到被计后 940 可抢占归属）；**同波段用光强度判定**（energy：枪端 30ms 双帧窗口，200=强光 100=弱光）
- **组播协调**：K 广播带 energy+channel 同步多网关仲裁表
- 服务器 `_killSeen` 仅记录归属（供 upgrade 转移），不做去重

## 3. 已知注意事项 / 坑

1. **ctf 夺旗按键已移除**（GPIO14 让位 I2S）——ctf 模式如要夺旗，需把夺旗键改接其他空闲引脚
2. **GPIO12 是 strapping 引脚**（枪端 850nm 发射、头盔 IMU 电源都用它）——电路必须加 10kΩ 下拉
3. **MPU9250 磁力计未校准**——yaw 为相对值；绕 8 字采集极值做硬铁/软铁补偿（待办）
4. **850nm 射程上限 ~20m（设计意图）**——固定低功率（独立限流 R3，不随
   powerLevel 变档）；20m+ 由 940nm 透镜通道承担。校准靶验证 20m 外不触发
5. 头盔 GPS 省电依赖 `V_BCKP 常接 3V3`（保 RTC 星历）——否则冷启动 1~3min
6. **光强度为双帧窗口代理**（非真实 ADC 光强）——如需精确光强，加光电二极管+ADC
7. `firmware/gun/src/Gps.h/.cpp`、`firmware/helmet/src/Display.h/.cpp`、`firmware/gateway/src/_draft.cpp` 是占位残留，可删
8. **RADIO_TX_POWER_DBM 22 超合规限值**：工信部 2019 年 52 号公告规定 470~510MHz
   微功率设备发射功率限值 50mW（e.r.p.≈17dBm），22dBm 传导 + 天线 e.r.p. 达 ~24dBm
   超限 → 正式版应降为 **15dBm 传导 + 2dBi（≈17dBm e.r.p.）**（1km 实测仍有余量，
   见 docs/wireless-research.md §6）

## 4. 待办 / 完善方向（按优先级）

### 近期（硬件调通）
- [ ] 打板/接线，逐端烧录验证（枪→盔→网关→服务器→控制台）
- [ ] 校准靶实测：10m（850nm 应触发）/ 20m（850nm 应**不**触发）/ 20m+（940nm 触发）
- [ ] 850nm 固定限流电阻 R3 校准（以"20m 外不触发"为目标，见 hardware-design §6.5）
- [ ] 磁力计 8 字校准（硬铁/软铁补偿），yaw 转绝对航向

### 中期（功能完善）
- [x] **无线升级 T1：单信道自适应 TDMA 固件（代码已就绪，待烧录实测）**——`firmware/lib/tdma`
      （TdmaMac/TdmaProto，FreeRTOS 任务）已接入枪/盔/网关三端：信标 + 广播下行窗 +
      设备时隙 + 注册时隙；开机负载均衡扫描自动分配信道（信标 N 最小者，平局随机，
      200 台/12 信道≈16-17 台/信道）；N 随活跃设备数自适应（超帧 30+10N ms）；
      SF7/500k/前导4/15dBm 已写入 config.h。待实测：注册收敛、时延随 N 变化、1km 丢包
      （前导 4/6/8 三档），方案见 docs/tdma-mac.md
- [x] **无线升级 T2：10 字节定长二进制帧协议（已实现）**——替换 ASCII 帧（protocol-915m.md
      为过渡版，语义保留）；布局 devIdx|type+flags|seq|payload5|crc8，帧类型 19 种，
      分片/重组（P 位置 3 片、W 规则 3 片），定义见 docs/protocol-tdma.md
- [x] **无线升级 T3a：网关开机自检 + 频率自动分配 + 信道质量检测（代码已就绪，待实测）**——
      槽位探测（SPI 读版本寄存器 0x0333 筛空槽）→ RSSI 统计测量标准栅格各频点质量
      （均值=噪声底 / 峰值=信号活动）→ 质量感知分配（均匀散布基线 + ±4 栅格局部替换，
      **自动避开干扰严重的频率**）→ 每射频独立 RadioLink+TdmaMac 实例，**没装满自动兼容**
      （装几个用几个，坏片跳过）；设备扫全栅格无缝工作。见 docs/tdma-mac.md §6
- [ ] **无线升级 T3b：多信道网关硬件**——12×SX1262 多射频板（槽位表 kRfSlots 填实际引脚）
      （渐进：1→6→12 射频，对应 17/100/200 设备）
      **算力核算见 docs/gateway-capacity.md**：CPU 够用（错峰+非阻塞 TX 后每核 ~40%），
      瓶颈在 RAM（任务栈须合并为 2-4 任务）与 GPIO（须换 ESP32-S3）；SPI 分 2 总线+互斥
- [ ] **无线升级 T4：多网关信标仲裁**——主/备信标网关切换 + 多网关共听 12 信道漫游
- [ ] **TX 功率降至 15dBm 传导（合规 17dBm e.r.p.）**——已写入 config.h，待实测复核覆盖
- [ ] **深睡模式**（SLEEP 0.6µA，唤醒 ~3.5ms）——当前 standby ~1.6mA，续航优化项
- [ ] W 帧友军位图映射修正（playerId→devIdx 正确映射，替代 T1 的相等假定）
- [ ] JOIN 携带设备名/master 关系（头盔配对，T1 已去除）
- [ ] 控制台回放：地图画朝向箭头（state 已带 yaw/pitch/roll，前端即取即用）
- [ ] 回放查看器页面（读 data/replays/*.jsonl 动画播放）
- [ ] 室内步数桥接（GPS 盲区用加速度计估算距离，协议加 accuracy 字段）
- [ ] 9 轴互补滤波（陀螺积分 + 磁力/重力校正，动态朝向平滑）
- [ ] 光强度升级：光电二极管 + ADC 实测光强（替换双帧窗口代理），同波段仲裁更精确
- [ ] 磁力计 8 字校准（硬铁/软铁补偿），yaw 转绝对航向
- [ ] 网关显示屏加音效/更多状态
- [ ] ctf 夺旗键改接（恢复夺旗模式）

### 远期（系统增强）
- [ ] **无改装水弹枪联动（研究完成 + 固件原型已实现，待硬件实测，见 docs/trigger-sensor-study.md + trigger-board.md）**——
      `firmware/trigger-board/`（INA226/ACS712 电流传感 + MOSFET 断电 + ESP32 + ESP-NOW）
      + 枪端 `GunEspNow` 集成（开火事件源 + 阵亡/空弹断电 + 重生/装弹恢复）；
      待办：电流波形采集校准阈值 → 硬件打样 → 端到端实测（延时/断电响应/多枪互扰）
- [ ] 枪端 OLED 改图标化界面 / 换彩屏
- [ ] 服务器 Web 端：战绩图表、玩家管理、多场地并发
- [ ] 回放轨迹热力图 / 击杀地图（含朝向）
- [ ] 电池电压监测 + 低电量上报
- [ ] OTA 固件升级（Web 推送）
- [ ] 加密链路（LoRa 加扰/鉴权，防伪造设备）

## 5. 快速启动验证（无硬件）

```bash
cd server && npm install && npm start          # 终端1
node tools/simulate.js --count 6 --gateways 2  # 终端2
# 浏览器打开 http://localhost:3000 看计分板/战术地图
```

> 模拟器已模拟：网关判定（newKill）+ 双波段 channel + 光强 energy + 多网关去重——
> 可验证服务器端计分与归属转移逻辑。真实网关判定链需烧录后对射验证。

## 6. 目录速查

```
docs/          架构/硬件/协议/模式/校准靶/户外可靠性/成本估算/本文件
firmware/gun/      枪端（红外+470M+OLED菜单+音效灯）
firmware/helmet/   头盔（4路红外+GPS+9轴+音效灯）
firmware/gateway/  基地台（470M⇄WS 桥接 + 组播 + OLED）
firmware/target/   校准靶（9路阵列）
server/            Node.js 服务器 + Web 控制台
tools/simulate.js  无硬件模拟器
```
