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
> | 4.4 `gateway` 用了 `setSPI()` 编译不过 | ✅ 已修 | 删掉该行；同时修好该文件另外 4 处历史遗留（`SX_SCLK`→`PIN_SX_SCLK`、`setPacketMode()`、`beginPacketMulticast`）→ `gateway` 现可编译（**2026-09-12 起按 ESP32-S3 编译：RAM 19.4% / Flash 28.4%**） |
> | 4.5 底噪测量用错 RSSI 口径 | ⏳ 未改 | 仍是 `getRSSI()`（包 RSSI）；本测试固件的扫频已用 `getRSSI(false)` 做对照 |
> | 4.6 `readData()` 早于 `getPacketLength()` | ✅ 已修 | `TdmaMac::readPacketPoll` + `gun/RadioLink::poll` + `gateway/RadioLink` 全部改为先查 IRQ、先取长度、显式处理 CRC 错 |
>
> 编译验证：`gun` / `gun-selftest` / `lora-gwtest` / `gateway` / **`helmet`** 五份
> **全部编译通过**（`helmet` 于 2026-09-12 修完，见下）。
>
> ✅ **`helmet` 已修（2026-09-12）**，共 5 处：
>
> | 文件 | 问题 | 修法 |
> | --- | --- | --- |
> | `helmet/src/RadioLink.cpp` | 用 `SX1262` 类（只认版本串 `"SX1261"`）→ 上板必 `-2 CHIP_NOT_FOUND` | 改 `SX1268`（并把 `getRadio()` 返回基类 `SX126x*`，与 gun/gateway/`TdmaMac` 一致） |
> | 同上 | 引脚宏名写成 `SX_NSS/SX_SCLK/...`（config.h 里是 `PIN_SX_*`） | 全部改 `PIN_SX_*` |
> | 同上 | `setSPI(&SPI)`：RadioLib 6.x 已移除 | 删掉，靠 `SPI.begin(明确引脚)` |
> | 同上 | `setPacketMode()` 无参调用：只适用 GFSK，本项目是 LoRa | 删掉（调用会返回 `WRONG_MODEM`） |
> | `helmet/src/RadioLink.cpp::poll()` | 先 `readData()` 后 `getPacketLength()`、且没先查 IRQ（会把上一包重复吐出来）——同 §4.6 | 与 `gun/RadioLink::poll()` 对齐：先查 IRQ → 先取长度 → `readData` → CRC 错重武装 |
> | `helmet/src/MotionSensor.cpp` | `motionISR` 被头文件声明为 `friend`（即 `extern`），定义却写成 `static` | 去掉 `static`（并加注释说明原因） |
> | `helmet/src/LedStrip.cpp` | `FastLED.addLeds<WS2812B, uint8_t, GRB>` 把"类型"当成了引脚模板参数；FastLED 3.10 数据脚必须是**编译期常量** | 改 `addLeds<WS2812B, PIN_LED, GRB>`（固定用 config.h 的 `PIN_LED`，传入的 pin 仅做校验并给出警告） |
>
> 同时按 gun/gateway 的方式加了 `selfCheck()`（读版本串/同步字/错误标志/瞬时 RSSI）。
> 编译结果：RAM 10.7% / Flash 28.2%（`esp32dev`）。

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

### 4.7 【经验·2026-09-12 实测】自研裸 SPI 驱动必发的三条命令 + 读寄存器偏移

> **结论先说**：这三条 **RadioLib 都会自动发**（call site 见下），所以 `gun`/`gateway`
> 用 RadioLib 的正式固件**不受影响、无需修改**。但凡是自己写 SPI 时序的（本仓的
> 测试固件 `lora-tx`/`lora-rx`/`spi-read`，或将来换驱动），**少任何一条都会出现
> "看起来全对但就是不通"的假象**。以下现象都是在真板子上逐条确认过的。

| # | 命令 | 少了它的现象 | RadioLib 在哪发 |
| --- | --- | --- | --- |
| ① | `SetDio2AsRfSwitchCtrl` `9D 01` | 发射 **`TX_DONE` 正常返回但空口上没有辐射**；接收端 RSSI **一个字节都不变地卡死**（看到的是被隔离的前端，不是天线）。原因：Ra-01S 的收发切换靠 **TXEN(脚5)/RXEN(脚11)** 的 RF 开关，本板 U2 只连 6 根线、没有这两个脚，只能由 **DIO2** 驱动（手册表 13-33：RX 时 DIO2=0、TX 时 DIO2=1） | `SX126x::begin()` → `setDio2AsRfSwitch(true)`（SX126x.cpp:192） |
| ② | `SetDioIrqParams` `08 03 FF 00 00 00 00 00 00` | `GetIrqStatus()` **恒为 `0x0000`**，轮询永远等不到 `TxDone`/`RxDone`（收端表现成"对端在发、本端什么都收不到"）。实测现象很迷惑：`irq=0x0000 air=200ms`（跑满超时），但**状态字节已报 `0x2C`，其中 bit3:1 = 6 = 「Command TX done」** —— 包其实发出去了，只是标志位没被记录。原因：手册 §13.3.2「**默认所有 IRQ 处于被屏蔽状态（全 0）**」 | 每次收发前重设：`startReceiveCommon()` 用 `RX_DONE\|CRC_ERR\|HEADER_ERR`、`startTransmit()` 用 `TX_DONE\|TIMEOUT` |
| ③ | `SetPaConfig` `95 04 07 00 01` + `SetTxParams` `8E 0F 04` | 不配 = **默认低功率 PA**（+14dBm 档）。高功率档见手册表 13-21：`paDutyCycle=0x04 hpMax=0x07 deviceSel=0x00 paLut=0x01`；本项目功率 +15dBm、ramp 200µs | `begin()` → `setOutputPower(RADIO_TX_POWER_DBM)`（SX1268.cpp：`setPaConfig(0x04, deviceSel=0x00)` + `setTxParams(power, PA_RAMP_200U)`） |

**另外两条读时序结论**（同样实测确认）：

- **返回数据的命令，命令/地址之后先有一个 `Status` 字节，数据在它后面**：
  `ReadRegister` 的数据从第 **4** 字节起（手册表 13-25 正文："主机必须在发送两字节的
  地址后发送一个 NOP，然后可以开始进行下一个 NOP 触发的数据接收"）；
  `GetPacketType` 的 packetType 在第 **2** 字节；`GetDeviceErrors` 的 OpError 在第 **2-3** 字节。
  按"数据从第 3 字节起"读就会**整体错位一个字节**（现象：每条第 1 个字节恒为 `0xA2`，
  而版本串内容恰好晚一个字节出现）。RadioLib 取的是 `buffIn[cmdLen + 1]`（Module.cpp:402，
  其 `widths[STATUS]` 保持默认 `BITS_8`）。
- 复位后默认 `pktType=FSK`、寄存器为表 12-1 的复位值
  （`0x0740=0x14` `0x0741=0x24` `0x08AC=0x94` `0x0911=0x05` `0x0912=0x05`），
  可以用"读到的值是否等于复位值"来验证 SPI 到底读对没有。

**已回流的改动**：`firmware/gun/src/RadioLink.cpp`、`firmware/gateway/src/RadioLink.cpp`
各加了 `selfCheck()`（开机自动跑，也可重复调用）：读 `0x0320` 版本串 + `0x0740/0x0741`
同步字 + `GetDeviceErrors` + 瞬时 RSSI，打印一行判据 —— 以后"LoRa 不通"在开机日志里
就能直接看出是 SPI、芯片错误标志，还是网络同步字的问题。

### 4.8 【2026-09-12】网关端换板与接线统一

- **网关主控由「ESP32 经典款」换成 ESP32-S3**（`platformio.ini`：
  `board = esp32-s3-devkitc-1` + `ARDUINO_USB_CDC_ON_BOOT=1`），并**改用与枪端完全相同的接线**：
  `NSS=G16 SCK=G42 MOSI=G15 MISO=G41 RST=G46 BUSY=G45 DIO1=G04`
  （`gateway/src/config.h` 与 `gun/src/config.h` 的 `PIN_SX_*` 已逐条一致，7/7 核对通过）。
- 换板时必须避开的 S3 脚：**G19/G20 = USB D-/D+**（原网关的 MISO 正好在 G19 —— 在经典 ESP32 上
  合法，在 S3 上会与 USB 外设打架，故随本次统一接线一并消除）、G26~G32 = Flash、
  G33~G37 = Octal PSRAM（R8 模组）、G0/G3/G45/G46 = strapping。
- `gateway/src/main.cpp` 新增 `checkPinsForS3()`：开机把 SPI 四线 + 槽位表逐脚核一遍，
  冲突就打印 `❌ 引脚冲突` 并给出原因（strapping 脚只给 ⚠ 提示）。
- 测试固件（`wire-probe` / `spi-read` / `lora-tx` / `lora-rx` / `spi-probe`）随之**去掉了
  单独的网关 env**：两块板接线相同，一律烧 `-e gun-s3`。
- **2026-09-13 补齐最后一处不一致：OLED 由 21/22 改为与枪端相同的 17/18**
  （`gateway/src/config.h` 的 `PIN_OLED_SDA/SCL`）。21/22 是**经典 ESP32 的默认 I2C 脚**，
  而 **ESP32-S3 根本没有 GPIO22**（S3 的 GPIO 只有 0~21、26~48，22~25 不存在），
  屏在 S3 上必然点不亮（不会崩：ESP-IDF 对非法脚只返回 `ESP_ERR_INVALID_ARG`）。
  两端 `Display.cpp` 实现相同（都是 `U8G2_SSD1306_128X64_NONAME_F_SW_I2C` 软件 I2C，
  构造参数为 `(U8G2_R0, scl, sda, U8X8_PIN_NONE)`），故只改引脚即可，代码零改动。
  至此网关与枪端的 7 根射频线 + SPI 四线 + OLED 两线**全部一致**。
- **2026-09-13 状态灯也对齐了**：网关原 `PIN_STATUS_LED 2`（经典 ESP32 的板载 LED 脚；
  在 S3 上 G2 是空脚，枪端把 G2 当按键用）→ 改为与枪端相同的 **`PIN_LED 48`**。
  ⚠️ 两点必须记住：① G48 的灯在**模组板上自带**、载板无需接线；
  ② 它是**普通单色 LED（不是 WS2812）**，灌电流接法 → **低电平点亮**，
  故用普通 `digitalWrite`、且极性反着写（网关里封装成 `statusLedWrite(bool)`）。
  枪端 `LedStrip` 也随之从 FastLED/WS2812 版改写为单色版（"颜色"→**闪烁节奏**：
  待机 1Hz 慢闪 / 存活常亮 / 阵亡 4Hz 快闪 / 结束 0.5Hz），接口不变、调用点无需改，
  `gun/platformio.ini` 的 `fastled` 依赖一并去掉。原 `LED_DATA=G08` 是**规划未接线**项，
  映射表已更正（`docs/gun-selftest-联调记录.md` 早就确认过 G48 才是实际那颗灯）。

### 4.9 【2026-09-12】正式固件"刷上去没输出" —— ESP32-S3 USB-CDC 把开机打印丢了

**现象**：测试固件（`lora-tx`/`lora-rx`）串口一切正常，但刷 `gun` / `gateway` 后
**打开监视器什么都没有**（板子其实在跑）。

**原因**：`platformio.ini` 里 `ARDUINO_USB_CDC_ON_BOOT=1` 时 `Serial` 走**原生 USB-CDC**；
**主机（串口监视器）打开之前，所有 `Serial.print` 全部被丢弃**。而这两个正式固件的打印
**只写在 `setup()` 里**，`loop()` 中一行串口都没有 → 开机那几百毫秒的输出正好落在
"主机还没连上"的窗口里，于是表现为完全没输出。测试固件因为**每秒都在打**，所以看得见。

**修法（两个固件都已落地）**：

| 措施 | 作用 |
| --- | --- |
| 开机 `for (t=millis(); !Serial && (millis()-t)<2500;) delay(20);` | 监视器已打开时，开机报告能打出来（无主机时最多等 2.5s，不阻塞电池单供） |
| `loop()`：`!hostSeen && (bool)Serial` → 补打一次 `printSerialReport()` | 后接入主机也能拿到完整状态 |
| `loop()`：每 5s 一行 `#hb` 风格状态 | **任何时候打开监视器，5s 内必有输出** |
| 网关 `g_rfCount==0` 的 FATAL 分支由 `while(1){}` 改成每 2s 打印一次原因 | 不再"静默死循环"，射频没起来时能直接看到要查什么 |

状态行示例：

```
[gun] t=12s radio=OK TDMA=run+locked ch=0 slot=3 devs=2 hp=100/100 ammo=120/120 alive=1 reg=yes
[gw]  t=12s rf=1 ip=192.168.1.50 ws=up | rf0 470.0MHz k=0 devs=2
```

**仍然没输出时的排查顺序**：① 打开监视器后**按板子复位键**（现在开机等主机 2.5s，复位后必见报告）；
② 确认接的是**原生 USB 口**（S3 上 GPIO19/20 那一路；UART 桥那一路没有 CDC 输出）；
③ 看状态 LED 有没有在闪（在闪 = 程序在跑，问题只在串口通道）；④ 复位原因/backtrace
（`CORE_DEBUG_LEVEL=2` 会打出来）——若是 `Brownout`/`TG0WDT` 之类，就是供电或死锁。

### 4.10 【2026-09-13】网关自检误报 "no radio up" —— 探针没驱动 RST 就位

**现象**：刷 `gateway`（`-e esp32s3`）后，串口每 2s 重复一行：

```
[self-test] FATAL: no radio up —— 查 PIN_SX_*(NSS=G16 SCK=G42 MOSI=G15 MISO=G41 RST=G46 BUSY=G45) / 3.3V 供电 / 模组焊接；本机已检测到 0 个槽位
```

`已检测到 0 个槽位` 是关键：它来自 `probeSlot()`，意思是**裸 SPI 读寄存器一个应答都没有**
（注意区分：不是"应答了但 `begin()` 失败"——那种情况 `m > 0`，FATAL 里会显示 1）。

**根因（代码缺陷，不是焊接/供电）**：`probeSlot()` 跑在 `RadioLink::begin()` **之前**，
而全工程**唯一会驱动 RST 的地方**是 RadioLib 的 `SX126x::reset()`（在 `begin()` 内部：
RST 输出低 1ms → 高 10ms）。所以自检这一刻 **G46 还停在上电默认态**：

- G46 是 ESP32-S3 的 **strapping 脚**，软件不配置时其电平由**芯片内部弱上拉/弱下拉**决定
  （ESP32-S3-WROOM-1 数据手册 §3.3 Strapping Pins："接高阻或不接时，strapping 脚的默认
  输入电平由内部弱上拉/下拉决定"）；
- SX1268 的 NRESET 只有**内部 ~50k 上拉、没有下拉**（数据手册表 8-3 `IN PU`）；
- 两者分压 **≈1.6V**，正好落在 NRESET 阈值不确定区 → 芯片可能一直停在复位态，
  SPI 完全不应答（MISO 读回 0x00/0xFF）→ `m=0` → FATAL。

**对照证据**：昨晚能读到版本串的 `spi-read`，正因为它开机**显式驱动了 RST**
（`spi-read/src/main.cpp:57-58` 输出高，`:315-317` 拉低 2ms 再拉高）。
两者的引脚完全相同，唯一的结构性差异就是"有没有驱动 RST"。

**顺手修掉的两个错**：

| # | 错 | 正 |
| --- | --- | --- |
| 1 | 探针读 **0x0333**（RadioLib 未定义该寄存器） | 版本串在 **0x0320**（`RADIOLIB_SX126X_REG_VERSION_STRING`，RadioLib `findChip` 从这读 16 字节） |
| 2 | 把 `rx[3]` 当"版本首字节" | **`rx[3]` 是状态字节**（实测恒 0xA2），数据从 `rx[4]` 起（`Module.cpp:402` 的 `buffIn[cmdLen+1]`）——正是 spi-read 踩过的偏移坑 |

**修法**（`gateway/src/main.cpp`）：`probeSlot(nss, rst, busy)` 现在
① 先 `pinMode(rst,INPUT)` 读一次原始电平并打印为 `before=`（用来判断 NRESET 是否本来就被压住）；
② `pinMode(rst,OUTPUT)` + 拉高，解除复位、消除分压不确定态；③ 发 2ms 复位脉冲；④ 等 BUSY 释放；
⑤ 读 0x0320 共 16 字节，按 `ver="SX1268…"` 判在位；⑥ **每次探测都打印原始字节**（不再只在成功时打）。

自检通过时新增的那一行就是硬判据：

```
[self-test] slot nss=G16 rst=G46(before=0) st=0xA2 ver="SX1268 V2F 2F02..." -> present
```

**若修正后仍是 `0 个槽位`**，才轮到硬件，按 FATAL 里新给的顺序查：
① 模组 3.3V（量模组 VCC-GND）；② **RES(G46) 对 GND 静态电压应 ≈3.3V**——若 ≈1.6V 说明
NRESET 仍被分压 → **在 NRESET 与 3.3V 之间加 10k 上拉**（这也是根治办法：MCU 复位、未初始化、
深睡时射频电平都不再不确定）；③ SPI 四线通断；④ 烧 `spi-read` 对照（能读到版本串 = 硬件没问题）。

### 4.11 【2026-09-13】网关 WiFi 段阻塞开机 —— 联调 LoRa 先整体关掉

**现象/隐患**：`gateway/src/main.cpp` 的 `setup()` 末尾是

```cpp
while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
```

而 `WIFI_SSID` / `SERVER_HOST` 还是占位值（`"your-wifi"` / `192.168.1.100`）
→ **永远连不上、永远出不来**：`setup()` 卡死在这里，`loop()` 一行都不跑，
连"每 5s 一行心跳"也看不到（表现为刷完固件只有开机段，然后一片安静）。

**处理（2026-09-13）**：新增总开关，联调期间整体关闭 WiFi/服务器：

| 开关（`gateway/src/config.h`） | 作用 |
| --- | --- |
| `GW_WIFI_ENABLE 0` | 不初始化 WiFi、不连 WebSocket、不发网关间组播；LoRa / TDMA / 串口全部照常 |
| `GW_MIRROR_UPLINK_SERIAL 1` | 把本该上报服务器的 JSON **镜像到串口**（`[up-json] {...}` 行） |

实现方式（`gateway/src/main.cpp`）——用包装函数而不是到处写 `#if`：

- `wsSend(const String &s)`：`GW_WIFI_ENABLE=1` 时 `ws.sendTXT(s)`；
  `=0` 时打印 `[up-json] %s`。**各业务上报（`sendDev` 与各事件）一行都不用改。**
  （形参必须是 `String`：调用点传的是 `serializeJson(doc, out)` 的产物，
  写成 `const char*` 会编译报 `cannot convert 'String' to 'const char*'`。）
- `localIpStr()`：状态行/屏幕要显示 IP，WiFi 关闭时返回 `"-"`。
- `mcastSend/mcastPoll`、`wsEvent`、联网段、`loop()` 里的 `ws.loop()` 整段 `#if` 包住；
  WiFi 关闭时 `mcastSend/mcastPoll` 提供空操作重载。
- 头文件顺序要动：把 `#include "config.h"` 提到 `#if GW_WIFI_ENABLE` **之前**，
  否则该宏还没定义。

**效果**：不再阻塞开机；不接服务器也能从串口看到 JOIN / ASSIGN / 心跳 / 击杀等上行内容。
**副作用（好的那种）**：WiFi + WebSockets + mbedTLS 不再链接，固件体积骤降 ——
RAM 63,504 B → **36,024 B**，Flash 949,797 B → **326,585 B**（省下约 620 KB flash / 27 KB RAM）。

恢复完整功能：`GW_WIFI_ENABLE` 改回 `1`，并把 `WIFI_SSID/WIFI_PASS/SERVER_HOST` 填成真实值。

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
- **链路排查四步固件**（按顺序烧，每步都有硬判据；都是"不依赖 RadioLib"或最小依赖）：
  1. `firmware/wire-probe/` —— **连线**：`NSS/SCK/MOSI/RST` 分别输出
     **10 / 100 / 500 / 1000 Hz** 方波（不用 SPI、无任何库），示波器核对焊盘。
  2. `firmware/spi-read/` —— **读寄存器**：`0x0320` 版本串、`0x0740/0x0741` 同步字、
     `RxGain`、trim、错误标志；开机先复位，读到的应等于手册表 12-1 的复位值。
     （`firmware/spi-probe/` 是更早的、面向示波器触发的一版，已被 step 2 取代。）
  3. `firmware/lora-tx/` —— **发端**：470MHz/SF7/BW500k，定时发 8 字节 `LRTESTnn`，
     打印 `TX_DONE` 与实测空口时间（理论 8.00ms）。
  4. `firmware/lora-rx/` —— **收端**：连续接收，打印 `RSSI/SNR/数据`，并按载荷序号
     算 **PER**；`n` 键可扫 460~480MHz 看底噪。
     （2026-09-12 实测：两块板 19/19 收全、0 CRC 错、`air=8.0xms`，链路验证通过。）
- 引脚定稿：`PCB/gun-board/main-board-S3引脚映射.md`
- 帧协议：`docs/protocol-tdma.md`｜MAC 规范：`docs/tdma-mac.md`
- 无线方案研究：`docs/wireless-research.md`｜链路预算：`docs/link-budget.md`
- 硬件体检 + 联调固件：`firmware/gun-selftest/`
  （T1 LED / T2 射频在位 / T3 ESP-NOW / **T4 自动连网关 + 每秒心跳** /
  **T5 940nm-38kHz 光路矫正发射：每秒亮 0.7s，最低功率**）
  —— **T4 就是本网关固件 GW 模式的对端**，一端烧本固件、另一端烧 gun-selftest，
  即可跑通"扫频→注册→时隙心跳"的完整链路
- 枪端自检联调记录：`docs/gun-selftest-联调记录.md`
