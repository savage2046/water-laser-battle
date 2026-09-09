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
