# 枪端自检固件联调记录（gun-selftest）

> 日期：2026-09-09 · 阶段：PCB 焊接完成后的逐项硬件体检（第一天）
> 板卡：`gun-board.eprj2 / 板子 main`，主控 **LCKFB-ESP32S3R8N8**（DIP-40，丝印 `Gxx`=GPIOxx）
> 目录：`firmware/gun-selftest`（独立体检固件，与正式固件 `firmware/gun` 互不干扰）

## 1. 今日完成

### 1.1 原理图管脚定义在线读取（eda.* 官方 API）

- 通过本地 Bridge 读取运行中的嘉立创 EDA 专业版当前工程 `gun-board` 的
  `main / ESP32主控 / P1` 页：U1 符号 40 脚位定义 + Protel2 网表。
- 结论落档：`PCB/gun-board/main-ESP32主控-U1管脚定义-在线读取.md`
  - U1 = LCKFB-ESP32S3R8N8，DIP-40 排针 1~40；
  - **GPIO48 = DIP 第 26 脚（符号名 G48），载板上未接线 —— 该 LED 在模组板自带**：
    灌电流接法（阳极→3.3V，阴极→限流电阻→GPIO48），**低电平点亮**；
  - LoRa/OLED/IR 已连线脚与 `firmware/gun/src/config.h` **全部一致**（交叉核对 ✅）；
  - 扳机 G06、WS2812 G08、电机 G07、I2S G09/11/12、光感 G10、按键 G01/02/21/40、
    940 功率档 G14 仍为空脚 —— 后续 T4~T8 需先在 EDA 补线。

### 1.2 gun-selftest 固件（v0.3.0，全部编译通过）

| 检测项 | 内容 | 判定输出 |
| --- | --- | --- |
| T1 | 板载 LED（GPIO48/G48）1Hz 闪烁 | 目视：每秒 1 次 |
| T2 | LoRa 模块连接（U2 Ra-01S/SX1268，SPI CS=G16 SCK=G42 MOSI=G15 MISO=G41 RST=G46 BUSY=G45；RadioLib begin + 寄存器自检） | `#RESULT lora=PASS/FAIL(begin=码)` |
| T3 | ESP-NOW 自动连接：先连 NVS「上次成功地址」，3s 未连 → promiscuous 抓包按 RSSI 扫描选最强探测；成功写回 NVS | `#RESULT espnow=PASS/FAIL(code=码)` |

工程结构：
```
firmware/gun-selftest/
├── platformio.ini      env esp32s3（board esp32-s3-devkitc-1, USB CDC, RadioLib ^6.5.0）
├── README.md           使用/判定/排查文档
└── src/
    ├── main.cpp        T1 恒闪 + 结果灯；T2/T3 开机各跑一次；报告/心跳；命令 x
    ├── EspNowAuto.h    ESP-NOW 自动连接模块（NVS 记忆 + 扫描）
    └── EspNowAuto.cpp
```

### 1.3 关键设计与踩坑记录（重要，避免重踩）

1. **板载 LED**：GPIO48 低电平点亮（灌电流），固件 `digitalWrite(48, LOW)`=亮；已按立创
   官方 wiki 确认，且原理图在线读取复核 G48 为模组自带 LED。
2. **串口丢输出问题（两轮修复）**：
   - 原设计只在开机打一次 → 后开监视器看不到 → 改为 `logf()` 全走
     `(bool)Serial` 门控（无主机自动静默不阻塞，可电池单供）+ 主机接入瞬间补打报告；
   - 仍看不到报告但能收到 5s 心跳 → 定位为 **USB CDC 重枚举/端点未就绪窗口期的
     打印被 USB 层丢弃** → 报告在接入瞬间打一次 + **首个 5s 心跳周期再复述一次**
     （心跳已被证实必达）→ 解决。
   - 附带：上电 LED 三闪，不接串口也能确认程序在跑。
3. **ESP-NOW 的 RSSI 限制**：Arduino core 2.0.17（espressif32 7.x / IDF 4.4）的
   ESP-NOW 接收回调**不带 RSSI** → “扫描选信号最强”用 **WiFi promiscuous 旁路抓包**
   实现（只统计 ToDS=0/FromDS=0 的直连数据帧 = ESP-NOW 互发帧，按源 MAC 统计
   平均 RSSI/帧数）。**对端必须真的在信道 1 发帧才量得到**；检测板 1Hz 心跳必能扫到。
   - 帧格式沿用仓库协议 `{0xA5,...}`，探测 type=0x20，信道 1，与检测板/espnow-verify 兼容。
4. **连接判定**：成功 = 单播探测收到链路层 ACK（`ESP_NOW_SEND_SUCCESS`）或收到该 MAC 的帧
   （与仓库 espnow-verify / trigger-board-selftest 的判定口径一致）。

## 2. 现场测试状态（待续）

- 串口**能收到 5s 心跳行 `#hb ... lora=...`** → 已确认固件在跑、USB CDC 链路通；
- 完整报告“看不到”的问题已在固件侧修复（见 1.3-2），**新版 v0.3.0 尚待上板复测**；
- T2 LoRa 的 `begin=` 结果与 T3 ESP-NOW 的 `#RESULT` **尚未拿到最终数据** ——
  下一轮先烧 v0.3.0 复测并记录两项结论。

## 3. 待办（下一步）

- [ ] 烧录 v0.3.0，确认完整自检报告可见（含 `[T2]` / `[T3]`）；
- [ ] 记录 T2 LoRa：`begin` 返回码、RSSI（区分 PASS / -2 无应答 / -16 回读不一致等）；
- [ ] 记录 T3 ESP-NOW：上检测板/espnow-verify 对端，看 `#RESULT espnow=...`；
      用串口命令 `x` 清记忆重测“扫描选最强”分支；
- [ ] T2/T3 通过后按 README §4 逐项加 T4 OLED → T5 扳机 → …（需先在 EDA 补 🆕 空脚布线）；
- [ ] 体检完成后恢复正式固件 `firmware/gun`（自检固件会覆盖正式功能）。

## 4. 常用命令

```bash
cd firmware/gun-selftest
pio run -e esp32s3 -t upload          # 烧录
pio device monitor -b 115200          # 串口（模组原生 USB-C 口）
# 串口输入 x = 清除 ESP-NOW 已保存地址并重启
```

## 5. 相关文件

- 固件：`firmware/gun-selftest/`（README.md 为完整使用文档）
- 管脚定义：`PCB/gun-board/main-ESP32主控-U1管脚定义-在线读取.md`
- 引脚映射定稿：`PCB/gun-board/main-board-S3引脚映射.md`
- 正式固件：`firmware/gun/`（参考实现与回刷目标）

---

## 6. v0.4.0 追加：T4 LoRa 网关联机 + T5 光路矫正（2026-09-10）

> 需求：枪端测试固件要**自动连接 LoRa 网关**、**每秒给网关发心跳**，
> 同时**每秒用最低功率发 940nm/38kHz 编码信号**用于光路矫正
> （后续追加要求：**光信号持续 0.7 秒**）。

### 6.1 新增内容

| 项 | 实现 | 文件 |
| --- | --- | --- |
| T4 | 扫 20 个标准频点找网关信标 → 注册槽发 `JOIN` → 收 `TF_ASSIGN`（信道/时隙）→ **每 1000ms 入队一个 `TF_HB`，在自身时隙发出**；掉线（5s 无信标）自动重扫，重连优先试上次频点 | `src/LoRaDev.h/.cpp` |
| T5 | 每 1000ms 一个周期：**光信号持续 700ms**（亮 0.7s / 灭 0.3s）；亮期间背靠背连发与 `LaserCodec` **逐位一致**的 40bit 编码帧；载波 38.46kHz；**功率档 0 = 最低**（近距 `00`=0.5×I_nom）；通道可选（近距 G47 / 远距 G13 / 两个都发） | `src/IrAlign.h/.cpp` |
| 配置 | 引脚/射频/TDMA 常量/心跳与光路参数集中到 `src/config.h` | `src/config.h` |
| 命令 | `h/i/s/j/k<N>/I/b/c0|c1|c2/x`（详见 README §2.6） | `src/main.cpp` |

任务布局：**T4 在 core1 优先级 5**（470M 时隙收发），**T5 在 core0 优先级 10**
（38.46kHz 载波半周期仅 13µs，必须避开同核抢占；两任务分核互不干扰）。

### 6.2 编译状态

`pio run -e esp32s3`：**通过**。RAM 13.6%（44,584B / 320KB）、Flash 21.8%（729,801B / 3.3MB）。

### 6.3 本轮踩坑 / 重要结论

0. **【致命·2026-09-10 定位并修复】RadioLib 类选错 → `begin=-2 CHIP_NOT_FOUND`**。
   `lora-gwtest` 上板首测即报 `begin=-2`。根因**不是焊接**：RadioLib 的 SX126x
   家族类用 `0x0320` 版本字符串校验身份（`findChip` → `strncmp` 6 字节），
   `SX1262` 类只认 `"SX1261"`（RadioLib 头文件原注释：所有 SX1262 都上报 SX1261），
   而 **U2 = Ra-01S = SX1268**，上报 `"SX1268"` → 10 次重试后返回 -2。
   三端固件此前一律 `new SX1262(...)` —— 这解释了"LoRa 链路标 ✅ 但 T2 从没拿到
   PASS"（见 §2）。修法：`gun`/`gateway` 改用 `new SX1268(...)`；`TdmaMac` 参数与
   成员类型改基类 `SX126x*`；两个测试固件改为**自动依次试 SX1262/SX1268/LLCC68**
   并在报告里打印命中的类与版本字符串。详见 `docs/lora-gateway-test.md` §4.0。
1. **不能用阻塞 `transmit()` 等 DIO1 电平：已由硬件解决（2026-09-10 DIO1 → G04）**。
   接上 DIO1 后阻塞 `transmit()` 恢复正常（~9.3ms 返回，而不是白等 ≈46ms 才
   `TX_TIMEOUT`）。两个测试固件仍坚持用 `startTransmit()` + 轮询 `TX_DONE`：
   ① 接没接 DIO1 都能跑；② 顺带实测空口耗时 ≈9280µs，可反证 DIO1 接好没有。
   同时用 DIO1 中断（上升沿 = RX_DONE）取**收包到达时刻 ±µs** ——
   T4 的信标相位 `到达时刻 − 9.3ms` 与时隙对齐偏移都以此为基准（原先轮询
   时间戳误差可达 ~1ms）。`_txWindow` 用来屏蔽发射期间（TX_DONE）的跳变。
   连带改线：**G04 原规划的 `IR_RX` 迁到 G14**，940 功率档取消
   （`PIN_IR_POWER 0xFF`，940 是 R447=47Ω 单档硬件）；
   `firmware/gun/src/config.h` 已同步，三份固件全部编译通过。
   原始分析（正式 `RadioLink.cpp` / `TdmaMac::txFrame()` 的 46ms 隐患、
   `gateway/RadioLink.cpp` 用了 RadioLib 6.x 已删除的 `setSPI()` 编译不过）
   见 `docs/lora-gateway-test.md` §4。
2. **T4 全程连续 RX**（不像正式 `TdmaMac` 那样窗口式武装/standby）：
   任一时刻收到信标都能重新锁相，被抢占也不会失锁。
3. **T5 放另一个核**：38.46kHz 载波只有在整段忙等、不被抢占时才能保持频率准确，
   否则 37.9kHz 带通的接收头解不出来。
4. **0.7s 常亮 = 背靠背连发帧**：解码器只在 `S_IDLE` 等"引导码结束"（9ms 电平），
   上一帧结尾的空闲不会误触发，所以连续帧可以逐帧反复解码 —— 既好目视对准，
   又能让接收端反复给出解码反馈。

### 6.4 待上板验证

- [ ] T4：`#RESULT gateway=PASS`，心跳间隔 ≈1000ms、时隙偏移 \|avg\|<1ms
- [ ] T4：网关侧（`firmware/lora-gwtest` 的 GW 模式）看到 `JOIN dev=1 "G0001"`、
      `dev=1` 设备表、HB 计数 ≈1/s
- [ ] T5：手机摄像头看到 LED 每秒亮 0.7s；示波器确认 38.46kHz + 9ms 引导码 + 40bit
- [ ] T5：接收头/校准靶能解出完整帧（校准靶蓝闪 3 次）；记录能解码的最大距离
- [ ] 用命令 `I` 暂停 T5，对比 T4 的心跳间隔/时隙偏移，确认两任务互不干扰

