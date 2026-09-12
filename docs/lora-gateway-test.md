# LoRa 网关链路测试（枪端 PCB 充当路由/网关）

> 日期：2026-09-10 · 状态：**固件已实现并编译通过（未上板实测）**
> 硬件：`gun-board.eprj2 / 板子 main`（LCKFB-ESP32S3R8N8 + Ra-01S/SX1268）
> 固件：`firmware/lora-gwtest/`（独立工程，见其 README.md 的完整使用文档）
> 目的：把焊接好的**另一块枪端 PCB** 当 LoRa 网关跑起来，验证射频链路与
> TDMA 时隙组网能力（对端为**已烧正式固件的枪端**）

## 1. 背景与目标

枪端 PCB 已焊好第二块，计划用作 **LoRa 网关/路由**。正式网关固件
（`firmware/gateway`）面向「ESP32 经典款 + 多射频 + WiFi/WebSocket 桥接」，
不适合作为**第一次上板的链路验证**工具：它一上来就连 WiFi 连服务器，链路问题
会被上层噪声掩盖。

因此新建独立测试固件 `firmware/lora-gwtest`，只做一件事：
**把 470MHz 链路跑起来并把每个指标量化打印**。

三项验证目标：

| # | 目标 | 对应模式 |
| --- | --- | --- |
| V1 | 这块 PCB 的 SX1268 收发链路本身是否正常 | SCAN / MON / PING |
| V2 | 与正式枪端之间的上行链路质量（PER、RSSI/SNR、连续丢包、间隔、时隙偏移、注册时延） | MON / GW |
| V3 | TDMA 时隙兼容性（能否作为网关把枪端接进来并稳定调度） | GW |

## 2. 交付物

| 文件 | 内容 |
| --- | --- |
| `firmware/lora-gwtest/platformio.ini` | env esp32s3、USB CDC、RadioLib、`lib_extra_dirs = ../lib`（复用 TdmaProto） |
| `firmware/lora-gwtest/src/config.h` | 引脚（同 gun-board）/ 射频参数 / TDMA 时隙常量 / 测试帧类型 / 接收窗裕量 |
| `firmware/lora-gwtest/src/Stats.h/.cpp` | 统计模块（每设备 + 全局 + Ping + 频点质量） |
| `firmware/lora-gwtest/src/LoRaTest.h/.cpp` | 射频引擎（FreeRTOS core1 prio5）：SCAN/MON/GW/PING/ECHO |
| `firmware/lora-gwtest/src/main.cpp` | 自检报告 / 串口命令 / LED / 心跳 / 逐帧日志 |
| `firmware/lora-gwtest/README.md` | 使用文档（编译烧录、命令、判定标准、报告解读、排查表） |

编译结果：`RAM 10.8% (35KB/320KB)`、`Flash 9.4% (313KB/3.3MB)`，无警告。

## 3. 设计要点

### 3.1 帧层面 100% 复用仓库协议

本工程 `lib_extra_dirs = ../lib` 直接编译 `firmware/lib/tdma/TdmaProto.cpp`，
即 10 字节定长帧（`devIdx|type+flags|seq|payload5|crc8`，CRC-8/ATM）与正式固件
**是同一份代码**，不存在"两套帧格式对不上"的风险。

### 3.2 五种模式（详见 README §4）

- **SCAN**：20 个标准栅格（470.0+k×2.0MHz）逐点测**瞬时** RSSI（均值/峰值）+ 驻留收帧
  → 找出枪端/网关实际在哪个信道（解决"不知道对端在哪个频点"这一首要问题）；
  扫完自动锁定占用最强的频点并转入 MON。
- **MON**：锁定单频点**只收不发**（真正的被动监听，不干扰现场），
  统计所有合法帧的 RSSI/SNR/载频偏差。
- **GW**：与 `TdmaMac::runGateway` 同结构的超帧（信标 10ms + 下行窗 10ms +
  N×设备时隙 10ms + 注册时隙 10ms），真实枪端会扫到本信标并 JOIN 上来。
- **PING/ECHO**：测试帧 `0x1E/0x1F`（仓库 0x14~0x1F 未占用），
  两块本固件板互测可量出真实 RTT；对正式枪端广播 PING 是安全的
  （枪端 `handleTdmaFrame` 有 `default: break;`）。

### 3.3 接收全部走寄存器轮询

gun-board **未引出 SX1268 的 DIO1**，所以本固件：
发射用 `startTransmit()` + 轮询 `getIrqStatus()` 的 `TX_DONE`；
接收同样轮询 `RX_DONE/CRC_ERR/HEADER_ERR`；
并严格按 RadioLib 文档在 `readData()` **之前**调用 `getPacketLength()`。
→ 副产品：能把**实测空口耗时**与 `getTimeOnAir(10)` 的理论值对比，直接暴露
"是否还在用阻塞 transmit"（见 §4.1）。

### 3.4 统计口径

| 指标 | 口径 |
| --- | --- |
| PER | 上行帧 `seq`（16 位自增）连续性：`缺失 / (收到 + 缺失)`；J 帧（seq 恒 0）与测试帧不计 |
| 连续丢包 | 相邻 seq 的缺口长度分布：1 / 2 / 3 / 4 / 5-9 / ≥10 |
| 时隙偏移 | `(本帧到达时刻 − 估算空口时长) − 期望时隙起点`（µs），只统计设备时隙内的帧 |
| 注册时延 | `ASSIGN 发射完成 → 该设备首个时隙帧到达`（µs） |
| 信标→帧 | 相对**实际信标空口起点**的时延（µs，含设备处理 + 单程空口） |
| RTT | PING 空口起点 → PONG 到达（原始），另给出**扣除双程空口**的净值 |
| 载频偏差 | `getFrequencyError()`（Hz），反映两端晶振/TCXO 对齐程度 |
| 实测信标周期 | 相邻两次信标空口起点之差（µs），用于暴露超帧跑偏 |

## 4. ⚠️ 测试固件开发中发现 / 需要修正的正式固件问题

> ### 📌 修正状态（2026-09-10 全部落地）
>
> | 条目 | 状态 | 落地方式 |
> | --- | --- | --- |
> | 4.1 阻塞 `transmit()` 白等 46ms | ✅ 已修（两处） | ① gun-board 硬件改线 DIO1→G04（枪端自动恢复）② `TdmaMac::txFrame` 改为**轮询 IRQ 寄存器**（多射频网关没有 DIO1 引脚预算，必须这样） |
> | 4.2 注册窗裕量 2000µs < 6ms 退避 | ✅ 已修 | `TdmaMac.cpp`：`T_SLOT_RX_MARGIN 1500` / `T_REG_RX_MARGIN 6000` |
> | 4.3 超帧周期误差累积 | ✅ 已修 | `TdmaMac.cpp`：相位基准改用**实际信标空口起点**、周期恒定 `30+10N + T_SF_TAIL(8ms)` |
> | 4.4 `gateway` 用了 `setSPI()` 编译不过 | ✅ 已修 | 删掉该行；同时修好该文件另外 4 处历史遗留（`SX_SCLK`→`PIN_SX_SCLK`、`setPacketMode()`、`beginPacketMulticast`）→ `gateway` 现可编译（RAM 19.5% / Flash 76.5%） |
> | 4.5 底噪测量用错 RSSI 口径 | ⏳ 未改 | 仍是 `getRSSI()`（包 RSSI）；本测试固件的扫频已用 `getRSSI(false)` 做对照 |
> | 4.6 `readData()` 早于 `getPacketLength()` | ✅ 已修 | `TdmaMac::readPacketPoll` + `gun/RadioLink::poll` + `gateway/RadioLink` 全部改为先查 IRQ、先取长度、显式处理 CRC 错 |
>
> 编译验证：`gun` / `gun-selftest` / `lora-gwtest` / `gateway` 四份**全部编译通过**。
> ⚠️ `helmet` **仍编译不过**（3 处，均与本批改动无关的历史问题：
> `MotionSensor.cpp` 的 `motionISR` 先 `extern` 后 `static`、`LedStrip.cpp` 的
> FastLED `addLeds` 模板参数与已装版本不匹配、`RadioLink.cpp` 与网关同款的
> `SX_SCLK` 命名错误）—— 需要时按同样方式修。

以下 7 项是在读代码 + 编译验证中发现的**真实隐患**（保留原始分析备查；
本测试固件已全部规避）。

### 4.0 【致命·已修】RadioLib 类选错 → `begin=-2 CHIP_NOT_FOUND`（LoRa 从未真正跑通过）

**现象**：上板烧 `lora-gwtest`，串口直接给

```
[T2] LoRa 模块初始化失败：begin=-2（CHIP_NOT_FOUND：芯片无应答 —— CS/MISO/BUSY/供电/焊接）
```

**根因**：RadioLib 的 SX126x 家族类用 **`0x0320` 版本字符串**做身份校验
（`SX126x::findChip()` → `strncmp(类名, 版本字符串, 6)`，不匹配就 reset 重试 10 次，
最后返回 `RADIOLIB_ERR_CHIP_NOT_FOUND` = **-2**）。而：

```cpp
// RadioLib/src/modules/SX126x/SX1262.h 第 14~16 行（原文）
// Note: this should really be "2", however, it seems that all SX1262 devices report as SX1261
#define RADIOLIB_SX1262_CHIP_TYPE   "SX1261"   // ← SX1262 类只认 "SX1261"
// SX1268.h: #define RADIOLIB_SX1268_CHIP_TYPE "SX1268"
// LLCC68.h: LLCC68 类只认 "LLCC68"
```

本项目的射频模组 **U2 = Ra-01S，芯片是 SX1268**（`PCB/gun-board/...-在线读取.md`、
`main-board-S3引脚映射.md`、`firmware/*/config.h` 注释全都写着 SX1268），
但**三端固件一律 `new SX1262(...)`** → 版本字符串回报 `"SX1268"`，与类名
`"SX1261"` 不符 → 永远 -2。

**这解释了为什么"LoRa 链路"一直标着 ✅ 却从没上板通过**：`gun-selftest` 的 T2
结果一直没拿到 PASS（见 `docs/gun-selftest-联调记录.md` §2），
而这个报错信息又会把人误导到"焊接/供电"上去查。

**修法（已全部落地）**：

| 位置 | 改法 |
| --- | --- |
| `firmware/gun/src/RadioLink.*` | `new SX1262` → **`new SX1268`**；`getRadio()` 返回基类 `SX126x*` |
| `firmware/gateway/src/RadioLink.*` | 同上（多射频同样受影响） |
| `firmware/lib/tdma/TdmaMac.h/.cpp` | 参数与成员类型 `SX1262*` → **`SX126x*`**（基类，兼容三种类） |
| `firmware/lora-gwtest`、`firmware/gun-selftest` | **自动依次试 SX1262 / SX1268 / LLCC68**（各自 `begin()`），命中即用；报告里打印"命中的类 + 实际版本字符串" |

> ⚠️ 注意：LoRa 的 `begin(freq, bw, sf, cr, syncWord, power, preamble, tcxo)` 只在
> **派生类**（SX1262/SX1268/LLCC68）上声明，基类 `SX126x` 只有 GFSK 版
> → 用基类指针调 begin 会编译报 "no matching function"。必须先建具体类型指针、
> 用它调 `begin()`，成功后再上转为 `SX126x*`。

**教训（诊断习惯）**：`-2 CHIP_NOT_FOUND` 现在有两个完全不同的可能——① 类选错
（软件）② 真·焊接/供电（硬件）。两个测试固件都改成"先自动试三个类、失败时明确
打印'已排除类选错'"，以后一眼可分。


### 4.1 【严重】DIO1 未接线时阻塞 `transmit()` 每次白等 ≈46ms —— ✅ 2026-09-10 已用硬件解决

> **进展（2026-09-10）**：gun-board 已把 SX1268 的 **DIO1 接到 G04**，
> 三份固件（`firmware/gun`、`firmware/gun-selftest`、`firmware/lora-gwtest`）
> 的 `PIN_SX_DIO1` 已改为 `4` 并全部编译通过 → **本条隐患在本板上消除**：
> 阻塞 `transmit()` 恢复到 ~9.3ms 正常返回。连带改线：G04 原规划的 `IR_RX`
> 迁到 G14，940 功率档置 `0xFF`（见 `PCB/gun-board/main-board-S3引脚映射.md`
> 的改线记录）。两个测试固件还额外用 DIO1 中断取收包到达时刻（±µs），
> 把信标相位/时隙偏移的精度从 ~1ms 提升到 µs 级。
> 下面保留原始分析，供**未接 DIO1 的板卡/迭代版**参考。

`firmware/gun/src/RadioLink.cpp::send()` 与 `firmware/lib/tdma/TdmaMac.cpp::txFrame()`
都调用 RadioLib 的阻塞 `SX1262::transmit()`，其实现是：

```cpp
while(!this->mod->hal->digitalRead(this->mod->getIrq())) {   // 等 DIO1 电平
  yield();
  if (millis() - start > timeout) { finishTransmit(); return TX_TIMEOUT; }
}
```

`Module(NSS, RADIOLIB_NC, RST, BUSY)` 的 IRQ 引脚是 `-1` → ESP32 `digitalRead(-1)`
恒返回 0 → **必然走满超时分支**：`timeout = getTimeOnAir(len)×5/1000 ≈ 46ms`
（10B @SF7/BW500k）。数据其实已发出，只是白白阻塞 46ms 且返回 `TX_TIMEOUT`。

对 10ms 时隙的 TDMA 是致命的：**网关发一个信标就吃掉 4.6 个时隙**，
设备时隙全部错位。**这是"正式 TDMA 从未上板实测过"最可能踩到的第一个坑。**

**修法**：改用 `startTransmit()` + 轮询 IRQ 寄存器（见测试固件 `LoRaTest::txRaw`），
并用报告里的"实测空口耗时"回归验证（应 ≈9280µs，而非 ≈46000µs）。

### 4.2 【严重】注册窗裕量 2000µs < JOIN 随机退避 6ms

`TdmaMac::tryJoin()` 里 `jitter = esp_random() % 6000`，即 JOIN 空口起点在
`regStart + [0,6)ms`，空口 9.3ms → **最晚 `regStart+15.3ms` 才收完**。
而网关 `readPacketPoll(f, regStart + T_REG + 2000)` 只听到 `regStart+12ms`
→ 抖动 >2.7ms 的 JOIN 全部丢失（≈55%）。靠"每 3 个超帧重试一次"（120ms）
慢慢收敛，注册时间被拉长到秒级。

**修法**：注册窗裕量 ≥5.3ms（测试固件用 5000µs → 收全 ~95%）；
或把 JOIN 退避改小（`% 6000` → `% 800`）。

### 4.3 【严重】超帧周期误差累积 → 设备每隔一个超帧才同步（表现成 50% 丢包）

`TdmaMac::runGateway()` 末尾 `_sfStartUs = sfStart + T_BE + T_DL + N×T_SLOT + T_REG`，
即**固定按标称 30+10N ms 递推**；而注册窗实际会超时 2ms（见 4.2），
于是每个超帧的起点比标称晚 2ms，**误差逐帧累积**（2ms → 4ms → …）。
设备端虽然每个信标都重新锁相（`lockFromBeacon`），但一旦网关信标晚到超过
设备信标窗（`sfStart+T_BE+3000`，即 +13ms），设备就**丢信标**、继续按旧栅格
守候 → 下一个超帧才重新同步 → 上行看起来"每隔一帧丢一批"。

**修法**：① 先修 4.2 让注册窗不超时；② 把周期写成恒定值
（测试固件：`30+10N + GW_REG_RX_EXTRA_US + 1ms`），并把**实测信标周期
min/avg/max** 打进报告作为回归指标。

### 4.4 【严重】`firmware/gateway` 使用了 RadioLib 5.x 的 `setSPI()`，6.x 已移除

`firmware/gateway/src/RadioLink.cpp`：

```cpp
_radio = new SX1262(new Module(_nss, _dio1, _rst, _busy));
_radio->setSPI(&SPI);          // ← RadioLib 6.x 已删除该接口
```

实测编译报错：`error: 'class SX1262' has no member named 'setSPI'`。
（`firmware/gun/src/RadioLink.cpp` 早已改成"默认使用全局 SPI 实例"并留了注释，
网关那份漏了。）**修法**：删掉这一行，靠 `SPI.begin(...)` 配置引脚即可。

### 4.5 【中】底噪/信道质量测量用错了 RSSI 口径

`SX126x::getRSSI(bool packet = true)` 默认返回**上一个包的 RSSI**（读 packet status
寄存器），不是瞬时 RSSI。要测底噪必须传 `false`（读 `GET_RSSI_INST`）。

- `firmware/gateway/src/main.cpp::measureFreqs()` 用 `r->getRSSI()`
  → 无包时读到的是 0/陈旧值（≈0 dBm）→ **所有频点都会被判为 NOISY**，
  「避开干扰频点」的分配逻辑实际失效；
- `firmware/gun-selftest/src/main.cpp` 的 T2 底噪读数同样是包 RSSI，
  README 里写的"通常 −90~−115dBm"不会成立。

**修法**：底噪采样一律 `getRSSI(false)`；包 RSSI 才用 `getRSSI()`（默认）。

### 4.6 【轻】`RadioLink::poll()` 先 `readData()` 再 `getPacketLength()`

RadioLib 头文件明确写着 *"getPacketLength method must be called BEFORE calling
readData!"*。`firmware/gun/src/RadioLink.cpp::poll()` 顺序相反，取到的可能是
上一包的长度（`readData` 内部会 `setBufferBaseAddress()` 重置缓冲指针）。

**修法**：先 `len = getPacketLength()`，再 `readData(buf, sizeof(buf))`。
另建议改为轮询 `getIrqStatus()` 的 `RX_DONE` 位，并**显式处理
`RADIOLIB_ERR_CRC_MISMATCH`**（现有实现把 CRC 错当成"非超时错误"就
`startReceive()`，其实数据已在缓冲里、可以计数）。

## 5. 与 `TdmaMac` 的差异一览（2026-09-10 后已基本对齐）

§4 的修正落地后，正式 `TdmaMac` 与本测试固件在**影响 TDMA 能否跑通的每一点上
都一致了**，只剩两处有意保留的差异：

| 项 | `TdmaMac`（gun/helmet/gateway 共用） | 本测试固件 | 说明 |
| --- | --- | --- | --- |
| 发射 | `startTransmit()` + 轮询 `TX_DONE` | 同 | ✅ 已对齐（4.1，多射频网关无 DIO1） |
| 收包 | 先查 IRQ → 先取长度 → `readData` → CRC 错重武装 | 同 | ✅ 已对齐（4.6） |
| 相位基准 | **实际信标空口起点** `bcStart` | 同 | ✅ 已对齐（4.3） |
| 注册窗裕量 | **+6000µs** | +5000µs | 差异有意保留：`TdmaMac` 不动设备的 0~6ms 退避幅度，取 6ms 全覆盖；测试固件取 5ms 便于对照 |
| 超帧周期 | 恒定 `30+10N + 8000µs` | 恒定 `30+10N + 6000µs`（=裕量5ms+让出1ms） | 都是"恒定、不累积漂移"；绝对尾部大小不影响兼容（设备每个信标重新锁相） |
| 收包到达时刻 | 轮询（`vTaskDelay(1)`，~1ms 粒度） | **DIO1 中断（G04）→ ±µs** | 测试固件精度更高（用于量测）；`TdmaMac` 多射频无法给每个射频一根 DIO1，故不改 |
| 帧格式 | `TdmaProto`（同一份代码） | 同 | 100% 兼容 |
| LED/报告 | 无 | 串口统计报告 + G48 节奏 | 测试固件专属 |

> 结论：**现在可以认为"测试固件跑通的链路 = 正式网关也应能跑通"**，
> 条件是 `gateway` 板的射频槽位表（`kRfSlots`）按实际硬件填对。
> 两处保留的差异只影响时序数值（几 ms 级），不影响兼容性；
> 但**正式固件仍需上板实测**（注册收敛、时隙偏移、PER 都是首次有可对照的基准）。

## 6. 待实测清单（需要上板）

- [ ] 单板自检：`#RESULT lora=PASS`；版本寄存器 `0x0320` 应为 `SX1262`/`SX1268`
- [ ] **实测空口耗时 ≈9280µs**（若 ≈46000µs 说明仍在走阻塞 transmit）
- [ ] 接收 RSSI 量级合理（近距离 −40~−70dBm）、载频偏差 <5kHz
- [ ] SCAN 找到枪端实际使用的频点（应与正式网关分配的 `k` 一致）
- [ ] MON：看到枪端 `HB`（10s 周期）与 `JOIN`
- [ ] GW：枪端成功 JOIN、收到 `ASSIGN`、进入时隙发帧
- [ ] 记录：注册时延、时隙偏移抖动、上行 PER、连续缺口分布
- [ ] 实测信标周期抖动（应 <5ms；用于回归 4.3）
- [ ] 距离衰减曲线：RSSI/PER 随距离变化，1km 档前导 4/6/8 对比
- [ ] 两块本固件板互测 PING/ECHO：RTT 净 ≈ 处理+驱动开销（应远小于 18.6ms 双程空口）
- [ ] 长时间（≥30min）压测：RX 计数无异常、无任务看门狗复位

## 7. 命令速查

```bash
cd firmware/lora-gwtest
pio run -e esp32s3 -t upload      # 烧录
pio device monitor -b 115200      # 串口
# 串口命令（行末回车）：h 帮助 | m 切模式 | m0..m4 指定 | k<0-19> 换频点
#                        s 重扫 | i 报告 | r 清零 | p[n] ping | l 逐帧日志
#                        t/e 广播 START/END | x 重启
```

## 8. 相关文件

- 固件与完整使用文档：`firmware/lora-gwtest/README.md`
- 引脚定稿：`PCB/gun-board/main-board-S3引脚映射.md`
- 帧协议：`docs/protocol-tdma.md`｜MAC 规范：`docs/tdma-mac.md`
- 无线方案研究：`docs/wireless-research.md`｜链路预算：`docs/link-budget.md`
- 硬件体检 + 联调固件：`firmware/gun-selftest/`
  （T1 LED / T2 射频在位 / T3 ESP-NOW / **T4 自动连网关 + 每秒心跳** /
  **T5 940nm-38kHz 光路矫正发射：每秒亮 0.7s，最低功率**）
  —— **T4 就是本网关固件 GW 模式的对端**，一端烧本固件、另一端烧 gun-selftest，
  即可跑通"扫频→注册→时隙心跳"的完整链路
- 枪端自检联调记录：`docs/gun-selftest-联调记录.md`
