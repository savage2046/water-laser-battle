# lora-gwtest：枪端 PCB 充当 LoRa 网关的 470MHz 链路测试固件

> 目标硬件：**gun-board.eprj2 / 板子 main**（"另一块枪端 PCB"），主控
> **LCKFB-ESP32S3R8N8**（ESP32-S3，DIP-40，丝印 `Gxx` = `GPIOxx`），
> 射频 **U2 Ra-01S / SX1268**。
>
> 与 `firmware/gun`（正式枪端）、`firmware/gateway`（正式网关）、
> `firmware/gun-selftest`（硬件体检）**互不干扰**：刷入本固件即进入
> 「LoRa 网关测试」模式，测完重新烧录正式固件即可。

本固件把这块板子跑成 **LoRa 网关角色**，用来验证：

1. 这块 PCB 的 **SX1268 射频链路**是否正常（收发、RSSI/SNR、载频偏差）；
2. 与**已烧正式固件的枪端**之间的 470MHz 双向链路质量
   （丢包率 PER、连续丢包分布、到达间隔、时隙偏移、注册时延）；
3. **TDMA 时隙兼容性**（信标 / 广播下行窗 / 设备时隙 / 注册时隙，与
   `firmware/lib/tdma/TdmaMac` 同结构），即"路由器"能否作为网关把枪端接进来。

---

## 1. 与 gun-selftest 的分工

| | gun-selftest | **lora-gwtest（本固件）** |
| --- | --- | --- |
| 目的 | 焊接完的硬件逐项体检 | 470MHz **链路**与 TDMA 组网能力测试 |
| 射频 | 只做 `begin()` 在位自检（T2） | 真正跑起来：扫频/监听/发信标/收发统计 |
| 输出 | `#RESULT lora=PASS/FAIL` | 逐帧 `#rx` + 完整链路统计报告 |
| 引脚 | 同（gun-board） | 同（gun-board，见 §3） |

---

## 2. 编译 / 烧录 / 串口

```bash
cd firmware/lora-gwtest
pio run -e esp32s3 -t upload     # 编译烧录
pio device monitor -b 115200     # 串口（模组原生 USB-C，USB CDC）
```

> 首次编译会下载 RadioLib（`^6.5.0`，与正式固件一致），需联网一次。
> 本工程通过 `lib_extra_dirs = ../lib` 复用仓库的 `TdmaProto`（10B 定长帧 +
> CRC-8/ATM），因此帧格式与正式固件**在代码层面就是同一份**。

### 串口输出时机

- 上电先 **LED 三闪**（约 0.7s，无串口也能确认程序在跑），随后开始 **20 频点扫描（约 7s）**；
- 打印全部经 `gwLog()`：**无 USB 主机时自动静默、不阻塞**（可电池单供）；
- 主机接入瞬间自动补打完整报告 + 命令帮助，之后每 5s 一行 `#hb` 心跳；
- 逐帧日志（`#rx` 行）默认开，用 `l` 命令可关（追求时隙定时精度时关掉）。

---

## 3. 引脚（与 `firmware/gun/src/config.h` 一致）

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| 板载 LED | GPIO48（G48） | 灌电流，**低电平点亮** |
| LoRa U2（Ra-01S/SX1268） | CS=G16 SCK=G42 MOSI=G15 MISO=G41 RST=G46 BUSY=G45 | 板上已布线 |
| SX_DIO1 | **G04（2026-09-10 已接线）** | 中断线：收包上升沿 → ISR 记到达时刻（±µs）。未接线时须写 `-1`，本固件自动退回轮询时间戳 |

射频参数与枪/盔/网关三端一致：`470.0MHz + k×2.0MHz`、`BW 500k`、`SF7`、`CR 4/5`、
`sync 0x12`、`15dBm`、前导 4、10 字节定长帧。

---

## 4. 五种工作模式

| 模式 | 名称 | 是否发射 | 用途 |
| --- | --- | --- | --- |
| `m0` | **SCAN** 频点扫描 | 否 | 逐个测 20 个标准栅格的瞬时底噪（均值/峰值）+ 驻留 300ms 收帧 → 找出枪端/网关在哪个信道；扫完自动锁定占用最强的频点并转 MON |
| `m1` | **MON** 被动监听 | 否 | 锁定单频点只收不发，统计所有 10B TDMA 帧 + RSSI/SNR/载频偏差；**验证这块板接收链路最干净的方式** |
| `m2` | **GW** TDMA 网关 | 是 | 跑标准超帧（信标 + 下行窗 + N×设备时隙 + 注册时隙），枪端会扫到本信标并 JOIN 上来 → 测通完整 TDMA 链路 |
| `m3` | **PING** 主动测试 | 是 | 每 500ms 发测试帧 `0x1E`（PING），等对端回 `0x1F`（PONG）→ 真·往返时延 RTT / 丢包率 |
| `m4` | **ECHO** 应答器 | 是 | 收到 PING 立即回 PONG。**两块本固件板互测**（一块 m3、一块 m4）即可量出真实 RTT，并与理论空口时延对比 |

> 默认开机流程：SCAN（~7s）→ 自动锁到有 TDMA 帧的频点 → MON 持续监听。

### 测试帧类型

`0x1E/0x1F`（PING/PONG）是仓库 19 种帧类型之外的**测试专用类型**（0x14~0x1F 未被占用）。
正式枪端固件 `handleTdmaFrame` 的 `switch` 有 `default: break;`，收到即忽略 ——
**用 PING 模式对着正在跑的枪端广播是安全的，不会扰动它的状态机**。

---

## 5. 串口命令（行末回车）

```
h / ?      帮助
m          循环切换模式 SCAN→MON→GW→PING→ECHO
m0..m4     直接指定模式
k<0-19>    设定频点栅格（470.0 + k×2.0 MHz），如 k7
s          重新扫频（自动回到 SCAN）
i          立即打印完整统计报告
r          清零全部统计
p          发 20 个 ping；p<n> 发 n 个；p0 连续 ping
l          逐帧日志（#rx 行）开/关
t / e      广播 TF_START / TF_END（可选：促使枪端进入对局、产生更多帧）
           ⚠ 会真实影响对端枪端状态；MON 为被动模式，t/e 无效
x          重启
```

### LED（G48）指示

| 模式 | 含义 |
| --- | --- |
| 上电三闪 | 代码已启动（不依赖串口） |
| 快闪（120ms） | **射频初始化失败**（看串口 `#RESULT lora=FAIL`） |
| 慢闪（1s 亮/1s 灭） | GW 模式（正在发信标） |
| 500ms 亮/灭 | SCAN / MON / PING / ECHO |
| 收帧瞬间短亮 30ms | 正在收到数据（链路活动指示） |

---

## 6. ⚠️ 两个必须知道的实现要点（也是本次测试发现的正式固件隐患）

### 6.1 DIO1 与阻塞 `transmit()`（2026-09-10 已接 G04 → 硬件级解决）

> **现状**：gun-board 已把 SX1268 的 DIO1 接到 **G04**（`config.h`：`PIN_SX_DIO1 4`），
> 所以下面这个隐患**在本板上已被硬件消除**。留着本节仍有用：
> ① 未接 DIO1 的板子/迭代版仍会踩；② 本固件保留轮询式发射，正好能用
> "实测空口耗时"反过来验证 DIO1 到底接好没有。

DIO1 **没有**接线时（`PIN_SX_DIO1 = -1`），RadioLib 的阻塞 `transmit()` 是这样等的：

```cpp
while(!this->mod->hal->digitalRead(this->mod->getIrq())) { ... }   // 读 DIO1 电平
if (millis() - start > timeout) { finishTransmit(); return TX_TIMEOUT; }
```

`getIrq()` 无效（引脚 -1）→ `digitalRead` 恒返回 0 → **每次发射都白等满
`5×空口时长 ≈ 46ms` 才返回 `TX_TIMEOUT`**（数据其实已经发出去了，只是白白阻塞 46ms）。

对一个 10ms 时隙的 TDMA 系统，46ms 的阻塞是致命的：`firmware/gun/src/RadioLink.cpp`
的 `send()`、`firmware/lib/tdma/TdmaMac.cpp` 的 `txFrame()` 都是这个写法
（`radio.transmit(...)`），**网关发一个信标就会把整个超帧冲掉**。
**接上 DIO1 后这两处自动恢复正常**（~9.3ms 返回 `NONE`），一行代码都不用改 ——
这是那个隐患最省事的解法。

本固件不依赖这一点：`LoRaTest::txRaw` 用 `startTransmit()` + 轮询 IRQ 寄存器等
`TX_DONE`（接没接 DIO1 都能跑），并顺带测出空口耗时：

```cpp
int16_t tx(uint8_t *buf, size_t len, uint32_t *airUs) {
  uint32_t t0 = micros();
  int16_t st = radio.startTransmit(buf, len);      // 非阻塞起发
  if (st != RADIOLIB_ERR_NONE) return st;
  uint32_t deadline = micros() + 150000;
  while (!(radio.getIrqStatus() & RADIOLIB_SX126X_IRQ_TX_DONE))   // 轮询 IRQ 寄存器
    if ((int32_t)(micros() - deadline) > 0) break;
  *airUs = micros() - t0;
  radio.finishTransmit();
  return RADIOLIB_ERR_NONE;
}
```

接收同理：轮询 `getIrqStatus()` 的 `RX_DONE/CRC_ERR/HEADER_ERR`，
且**严格按 RadioLib 文档在 `readData()` 之前调用 `getPacketLength()`**
（`RadioLink.cpp` 现在是之后调用，长度可能取到上一包的值）。

> **到达时刻的两种精度**（接好 DIO1 后自动用前者）：
> - **已接 DIO1（当前）**：上升沿进 ISR 记 `micros()` → **±µs**，直接作为
>   `RxEvt::us` → 信标相位、时隙偏移、`respUs` 全部提升到 µs 级；
> - 未接：退回"轮询到包的时刻"（忙等轮询 ~50µs，`vTaskDelay(1)` 轮询可达 ~1ms）。
>
> 注意 `startReceive()` 只把 **RX_DONE** 路由到 DIO1，所以 **CRC/报头错的包不拉
> DIO1**，那类包会退回轮询时刻（只影响日志，不影响链路统计）。
> 另：`startTransmit()` 会把 DIO1 改成 TX_DONE，所以 ISR 里用 `_txWindow` 把
> 发射期间的跳变屏蔽掉。

### 6.2 超帧周期必须恒定，注册窗必须给足裕量

原 `TdmaMac::runGateway()` 有两处会让"网关 vs 设备"相位跑偏：

1. **注册窗裕量只有 +2000µs**，而设备 `tryJoin` 带 **0~6ms 随机退避**：
   JOIN 最早在 `regStart` 起发、最晚 `regStart+6ms`，空口 9.3ms → 最晚
   `regStart+15.3ms` 才收完。裕量 2ms → **抖动 >2.7ms 的 JOIN 全部丢**（≈55%），
   靠"每 3 个超帧重试一次"慢慢收敛。本固件把注册窗裕量提到
   `GW_REG_RX_EXTRA_US = 5000`（可收全 ~95%）。
2. **超帧周期按 `sfStart + 30+10N` 递推，而注册窗又超时 2ms** → 每个超帧晚 2ms、
   **误差逐帧累积**。设备端虽然每个信标都重新锁相，但一旦网关信标晚到超过
   13ms（设备 `readPacketPoll` 的信标窗是 `sfStart+T_BE+3000`），设备就丢信标、
   转而按旧栅格守候，于是**每隔一个超帧才同步一次**，表现成"50% 丢包"。
   本固件改为**恒定周期** `30+10N + GW_REG_RX_EXTRA_US + 1ms`，并用
   **实测信标周期 min/avg/max** 把它打进报告（抖动 >10ms 会直接标红提示）。

另外本固件在**每个超帧末尾固定 `vTaskDelay(1)`**：所有窗口都是忙等轮询，
若一个超帧内完全不让出 CPU，core1 的 IDLE 任务 5s 内跑不到 → **触发任务看门狗复位**。
（`TdmaMac` 靠 `taskEntry` 里每超帧一次的 `vTaskDelay(1)` 幸免，本固件的循环结构
不会返回，所以必须显式让出。）

---

## 7. 判定标准与报告解读

`i` 命令（或主机接入时）会打出完整报告，结构如下：

```
[lora-gwtest] ====== LoRa 网关链路测试报告 ======
  MCU=ESP32-S3  Flash=8MB  SDK=...  运行=42s
  [T1] 板载 LED（G48，低电平点亮）—— 程序运行中
  [T2] LoRa 模块 OK
       芯片=SX1262  版本寄存器 0x0320="SX1262"
       配置 470.0MHz BW500k SF7 CR4/5 sync=0x12 15dBm 前导4  → begin=0；理论空口(10B)=9280us
       引脚 CS=G16 SCK=G42 MOSI=G15 MISO=G41 RST=G46 BUSY=G45（DIO1 未接线→寄存器轮询）
#RESULT lora=PASS
  [扫频] 20 点标准栅格 ...
    k= 0  470.0MHz  底噪 avg=-104.3 max= -97 dBm  帧= 12  <== 有合法 TDMA 帧
    ...
    结论：占用频点 k=0 (470.0MHz)，12 帧 → 建议 GW/MON 用该频点
  [运行] 模式 = GW(TDMA网关)；频点 k=0 (470.0MHz)；当前底噪 = -104.3 dBm
         TDMA 超帧 = 30+10N = 40ms；mapVer=3 N=1；信标已发 1520；下行 3（ASSIGN 3）
         设备表：1 台  dev1→slot0
         实测信标周期 min/avg/max = 44980/45020/45100 us (n=1519)（标称 40000us + 注册窗裕量 5000us）
  [设备统计] 共 2 台
    dev=1   G0001   帧=42 HB=40 FIRE=2
         RSSI -62/-71 dBm (avg -66.5)  SNR 3.50/8.25 dB (avg 6.02)  载偏 avg -1200 Hz
         上行 seq 首/末=12/98 收=87 缺=0 → PER 0.00%  最大连续缺口=0  重复=0 复位=1
         连续缺口分布(gap=丢失帧数) 1:0 2:0 3:0 4:0 5-9:0 >=10:0
         到达间隔 min/max/avg = 9932/10045/10002 ms (n=40)
         时隙 slot=0 偏移 min/max/avg = -320/880/210 us (n=41)
         信标→本设备帧 min/max/avg = 24300/24800/24500 us (n=41)
         注册时延（ASSIGN 发完 → 首个时隙帧）= 41650 us
    dev=255 -       帧=1519 BEACON=1519        ← 若出现：这是"另一个网关"的信标
  [接收] 合法帧=42  LoRa CRC 错=0  长度异常=0  CRC8 错=0  事件丢弃=0
  [发射] 帧=1521 失败=0  空口耗时 min/avg/max = 9280/9312/9380 us（理论 getTimeOnAir(10) = 9280 us）
         信标=1520  下行=3（ASSIGN=3） START/END=0/0
  [Ping] 发=20 收=19 丢=1 未匹配=0 → 丢包率 5.00%
         RTT min/avg/max/P95 = 41.20/45.31/61.20/58.00 ms（原始，含双程空口 ≈18.6ms）
         RTT 净（扣除双程空口）= 22.60/26.71/42.60/39.40 ms
```

### 关键指标怎么读

| 指标 | 正常 | 异常 → 排查方向 |
| --- | --- | --- |
| `版本寄存器 0x0320` | `SX1262`（或 SX1268） | 空/乱码 → SPI 六线虚焊（配合 `begin` 返回码） |
| 空口耗时 | ≈9280µs（±200µs） | ≈46000µs → 仍在用阻塞 `transmit()`（见 §6.1） |
| 底噪（无信号频点） | −100 ~ −115 dBm | 明显偏高 → 干扰/天线异常/供电噪声 |
| 接收 RSSI | 近距离 −40~−70 dBm | 比预期低 20dB 以上 → 天线未接/匹配网络/PA 供电 |
| SNR | SF7/BW500k 下远距离 ≥ −7.5dB 可解 | 持续 ≤ −10dB → 距离/遮挡到极限 |
| 载频偏差 | \|avg\| < 5kHz | 偏大 → 晶振/TCXO 差异（SX1268 一般 <2kHz） |
| 上行 PER | 近距离 0% | >1% → 时隙碰撞（多网关同频）/功率不足/天线 |
| 时隙偏移 | \|avg\| < 1ms，抖动 <2ms | 抖动 >20ms → 对端定时被打印/中断打断 |
| 注册时延 | 40~120ms（1~3 个超帧） | 长期不注册 → 见 §6.2（注册窗裕量） |
| Ping RTT 净 | 30~40ms（两板同场） | 偏大 → 对端处理/驱动开销；丢包多 → 见 PER 排查 |
| 实测信标周期 | = 标称 + 裕量 + 1ms，抖动 <5ms | 抖动大 → DL 队列/串口打印负载 |

### 常见失败现象

| 现象 | 原因 / 处理 |
| --- | --- |
| **`#RESULT lora=FAIL (begin=-2)`** | `-2 = CHIP_NOT_FOUND`。**先看报告里有没有"已自动试过 SX1262/SX1268/LLCC68"这句**：<br>• 本固件**自动依次试三个类**（RadioLib 用 0x0320 版本字符串校验身份：SX1262 类只认 `"SX1261"`、SX1268 类只认 `"SX1268"`；U2 是 Ra-01S = **SX1268**，拿错类必然 -2）→ 所以出现这句就说明**不是类选错**，是硬件：U2 供电 3.3V/GND、CS=G16→U2.NSS、BUSY=G45（悬空/拉死会命令超时）、RST=G46、SCK/MOSI/MISO 虚焊错位、U2 贴反。<br>• 详见 `docs/lora-gateway-test.md` §4.0（这个坑曾让 LoRa 从未真正跑通过） |
| `begin=-16` | `SPI_WRITE_FAILED`：MOSI/MISO/SCLK 虚焊错位 |
| `begin=-705/-707` | 命令超时/失败：多半 BUSY(G45) 或 RST(G46) |
| 扫频全是"无帧" | 对端没上电 / 太远 / 天线未接 / 对端频点不在标准栅格（改 `TDMA_CHANNELS` 的临时配置） |
| 扫到"有能量但解不出帧" | 同频非本协议 LoRa（LoRaWAN/其它项目）或强干扰 → 换 `k` |
| MON 能看到枪端帧，但 GW 模式下枪端不 JOIN | 枪端已在别的网关注册且不会主动重扫 → **先开本固件（GW），再给枪端上电/复位**；或先用 `k` 换到干净频点 |
| GW 模式出现 `dev=255` 且有大量 BEACON | 正式网关也在同频发信标（双信标，T3c 仲裁未实现）→ 关掉正式网关，或换频点后重启枪端 |
| `#hb` 心跳不出现 | 射频任务在忙等中过多占用 core1（GW 模式 N 大时正常），或串口主机未接入 |

---

## 8. 推荐测试流程（与枪端正式固件联调）

1. **单板自检**：只插这块板，烧本固件，看 `#RESULT lora=PASS` + 空口耗时 ≈9.3ms
   → 证明这块 PCB 的射频焊接/供电/天线正常。
2. **被动监听**：把枪端（正式固件）上电，本板停在 SCAN→MON，
   看 `#rx` 是否出现枪端的 `HB`（每 10s 一次）与 `JOIN`；
   记录 RSSI/SNR/载频偏差 → 证明**接收**链路正常。
3. **TDMA 组网**：关掉正式网关（避免双信标），本板 `m2` 进 GW，**再复位枪端**；
   枪端扫到本信标 → 在注册槽发 `JOIN` → 本板回 `ASSIGN` → 枪端进入时隙发帧。
   观察 `#note JOIN dev=…`、`设备表`、`注册时延`、`时隙偏移`、`PER`。
4. **双向验证**：`p` 发 ping（对端为另一块本固件板时才有 PONG）；
   或 `t` 广播 START 让枪端进对局，产生更多 `FIRE/HB` 帧压测上行。
5. **距离/环境**：拉到目标距离，看 PER/连续缺口分布/RSSI 随距离的变化，
   记录 1km 档的前导 4/6/8 对比数据（与 `docs/wireless-research.md` 呼应）。

---

## 9. 文件结构

```
firmware/lora-gwtest/
├── platformio.ini       env esp32s3（board esp32-s3-devkitc-1、USB CDC、RadioLib、../lib）
├── README.md            本文件
└── src/
    ├── config.h         引脚 / 射频参数 / TDMA 时隙常量 / 测试帧类型 / 统计规模
    ├── Stats.h/.cpp     统计：每设备（RSSI/SNR/载偏/PER/间隔/时隙偏移/注册时延）
                         + 全局（收发计数、空口耗时、信标周期、Ping RTT/丢包、频点质量）
    ├── LoRaTest.h/.cpp  射频引擎（FreeRTOS 任务 core1 优先级 5；事件环形缓冲）
    └── main.cpp         开机自检报告 / 串口命令 / LED / 心跳 / 逐帧日志
```

> 统计不加锁（射频任务写、主任务读）：面向人读的诊断报告，允许 ±1 帧级别的
> 并发偏差，不会误导结论；表项只增不删，遍历安全。`r` 命令清零。

## 10. 相关文件

- 引脚定稿：`PCB/gun-board/main-board-S3引脚映射.md`
- 管脚在线读取：`PCB/gun-board/main-ESP32主控-U1管脚定义-在线读取.md`
- 硬件体检固件：`firmware/gun-selftest/`
- 正式枪端：`firmware/gun/`　正式网关：`firmware/gateway/`
- 共享 TDMA 帧协议：`firmware/lib/tdma/TdmaProto.h` + `docs/protocol-tdma.md`
- TDMA MAC 规范：`docs/tdma-mac.md`
- 联调记录与发现：`docs/lora-gateway-test.md`
