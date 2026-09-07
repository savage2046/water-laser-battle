# trigger-board-selftest：trigger-board（ESP32-C3 检测板）硬件自检固件

专用于 **triger-sensor 正式板**（`PCB/triger-sensor.eprj2`，主控
**ESP32-C3-WROOM-02-N4**，原生 USB-C 烧录/串口）的**独立硬件体检固件**。
与正式 `firmware/trigger-board` 互不干扰：刷入本固件即进入自检模式，体检完成
后重新烧录正式固件即可恢复正常工作。

> 目标：**检测硬件是否能够正常工作**，逐项给出 PASS/FAIL 结论：
>
> | # | 检测项 | 覆盖硬件 |
> | --- | --- | --- |
> | **1 电流检测** | INA226(0x40) 在位/ID/配置/读数 + 2mΩ 分流换算电流（噪声/极值/芯片交叉校验/空闲基线） | U3 INA226、R1 分流、R2/R3 上拉、IO4/IO5 I2C |
> | **2 ESP-NOW 连接** | 双向链路：单播心跳链路层 ACK（TX）+ 收到对端帧（RX） | ESP32-C3 射频、天线、与对端（枪端主控 / espnow-verify / 另一块同固件板）互通 |
> | 附：MOSFET 步进 | 门极 IO3→UCC27517→Q1 通/断引起回路电流变化（需台架负载） | U4 栅极驱动、Q1(20N03)、回路接线 |
> | 附：扳机监控 | 扣扳机时按正式固件阈值统计电流脉冲（命令 `c`） | 整条“电流检测→识别”链路 |

---

## 1. 硬件与接线

正式板自身无需额外接线（USB-C 供电即可完成 T1 电流检测链路 + T3 ESP-NOW 初检）：

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| INA226 SDA / SCL | IO4 / IO5 | 地址 0x40（A0/A1 接地），2.2kΩ 上拉在板上 |
| MOSFET 控制 | IO3 | → UCC27517 → Q1(20N03)，HIGH=回路导通 |
| 状态 LED | IO0 | 高电平点亮（自检结论指示，见 §6） |
| 串口 | IO18/IO19 | 原生 USB（Type-C），USB CDC 115200 |

### 台架负载（T2 MOSFET 步进用，可选）

T2 要验证“门极通/断 → 回路电流通/断”，需要回路里有一个**常通负载**：

- 电池接 XT30 电池口（2S/3S，7.4~12.6V），电机口接**台架负载**：
  12V 车灯/灯泡，或大功率电阻（如 4.7Ω/10W ≈ 2.3A@11.1V）。
- 不接负载也能跑，T2 会打印 `SKIP`（不影响 T1 结论）。
- ⚠️ 不建议用真枪做 T2：门极 ON 期间扣着扳机才会转，容易误射。

---

## 2. 编译 / 烧录 / 串口

```bash
cd firmware/trigger-board-selftest

pio run -e esp32c3 -t upload            # 正式板（原生 USB，默认环境）
pio run -e esp32c3-uart -t upload       # UART 桥 C3 开发板（如 DevKitM-1 板载桥）

pio device monitor -b 115200            # 看自检输出
```

> 两个环境共用一套代码，区别只是串口路由：
> `esp32c3` = `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`，
> `Serial` 走 IO18/19 原生 USB（正式板用）；
> `esp32c3-uart` = `Serial` 走 UART0（IO20/21，桥接板用）。
>
> 建议**先打开串口监视器再上电/复位**，避免漏掉开机自检输出；
> 串口后插/后打开时固件会自动补打一次结果汇总（`#serial connected`）。

## 3. 开机自动自检

上电后自动依次执行并打印：

1. **T1 电流检测链路**（无需外设）—— 判 PASS/FAIL
2. **T2 MOSFET 步进**（无台架负载自动 SKIP）
3. **T3 ESP-NOW 连接**（后台持续 1Hz 心跳 + 状态判定，观察窗口 10s）

随后打印 `#RESULT ...` 汇总。任何时刻可键入命令重跑单项（见 §5）。

### 对端准备（T3 ESP-NOW 需要）

T3 是“双向连接”测试，需要一个能回应的对端，任选其一：

| 对端 | 做法 |
| --- | --- |
| **另一块本固件板** | 两块都刷本固件 → 上电即自动发现（广播心跳+自动配对），约 1~2s 内互相确认，双向 PASS。 |
| **espnow-verify 板**（推荐，已有） | `firmware/espnow-verify` 刷在 S3/经典 ESP32 上。默认伪装成枪端 MAC `{0x24,0x0A,0xC4,0,0,1}`，把本固件 `config.h` 的 `PEER_MAC` 填成该值即可开机单播确认；对端串口会持续打印 `#HB`。 |
| **枪端主控（正式固件）** | 把 `config.h` 的 `PEER_MAC` 填成枪端主控真实 MAC；本板单播心跳被其 ESP-NOW 链路层 ACK → 单向确认（枪端固件不回传，T3 判 PARTIAL 属正常）。 |
| espnow-verify 不伪装 | 在 espnow-verify 侧按 `p0`/`p1` 广播下行指令，本板收到即自动配对 → 双向确认。 |

---

## 4. 结果判定

| 输出 | 含义 |
| --- | --- |
| `#RESULT current_detect=PASS` | INA226 链路完好（0x40 在位 + ID 正确 + 配置写读回一致 + 读数在量程/噪声达标 + 芯片与换算电流一致） |
| `#RESULT current_detect=FAIL` | 见输出中的 `#T1 reason=…`（多为焊接/上拉/供电问题） |
| `#RESULT espnow=PASS(双向)` | TX 被对端 ACK **且**收到对端帧 —— 连接正常 |
| `#RESULT espnow=PARTIAL(仅上行ACK)` | 本板能发出且对端在场，但对端不回传（枪端固件即此情形）；RX 路径未获确认 |
| `#RESULT espnow=FAIL(无对端确认)` | 观察窗口（10s）内无任何确认：对端没上电 / 信道不一致 / 射频故障 |
| `#RESULT espnow=FAIL(init)` | `esp_now_init` 失败（罕见，WiFi 栈问题） |
| T2 `PASS / SKIP / FAIL` | 需台架负载；SKIP 不影响整体结论 |

## 5. 串口命令

| 命令 | 作用 |
| --- | --- |
| `?` / `h` | 帮助 |
| `a` | 全部自检重跑（T1+T2） |
| `1` | 重跑 T1（INA226 电流检测） |
| `2` | 重跑 T2（MOSFET 步进，需台架负载） |
| `3` / `s` | ESP-NOW 状态 / 自检结果汇总 |
| `g0` / `g1` | 回路断电 / 导通 |
| `c` | 扳机电流脉冲监控开关（自动 `g1`，扣扳机看 `#PULSE`；`c`/`q` 退出） |
| `i` | 周期实时电流/总线电压打印开关（1Hz） |
| `r` | ESP-NOW 统计清零 + 删除自动配对（保留 config 固定对端） |
| `v` | 详细 RX 帧打印开关 |
| `b` | 空闲基线重校准 |
| `m` | 本机信息（芯片/版本/MAC） |

## 6. LED（IO0）指示

| 模式 | 含义 |
| --- | --- |
| 上电三闪 | 固件启动 |
| **常亮** | T1 电流检测 FAIL（或 esp_now init FAIL） |
| 1s 亮/1s 灭 | T1 PASS，ESP-NOW 观察期内等待对端 |
| 每 4s 短亮 150ms | T1 PASS，10s 后仍无对端确认 |
| 每 4s 双闪 | T1 PASS，ESP-NOW 仅单向（对端不回传） |
| **慢闪 500ms 周期** | **全部通过（电流 PASS + ESP-NOW 双向 PASS）** |
| 扳机监控时每发快闪 40ms | 电流脉冲事件（与串口 `#PULSE` 同步） |

> 电池单独供电（无 USB）时串口静默，直接看 LED 即可判断：
> 常亮=电流链路坏；慢闪=全过；其余见上表。

## 7. 安全说明

- 本固件开机默认 **MOSFET 断电**（门极 OFF、回路不通），比正式固件更保守；
  需要导通时用 `g1` 或命令 `c` 显式打开。
- 测试时请勿让水弹枪上弹；T2/监控建议只接台架负载或空枪。
- 体检完成后刷回正式固件：`cd firmware/trigger-board && pio run -t upload`。

## 8. 常见问题

- **`#T1 reason=INA226 无应答`**：查 U3 焊接/虚焊、R2/R3(2.2kΩ) 上拉是否贴装、
  VS+(3.3V) 是否到位、SDA=IO4/SCL=IO5 通路、INA226 地址是否 0x40。
- **ID 不符**：0x40 上可能贴错芯片；读回 0xFFFF 多为虚焊/总线错位。
- **读数噪声大/超量程**：查 R1(2mΩ) 焊接与阻值、分流采样走线、板载共地。
- **VBUS≈0V**：USB 单独供电时正常（无电池电压）；接 2S/3S 电池应 ≈7.4~12.6V。
- **ESP-NOW 收不到**：先 `m` 看本机 MAC 与信道（须与对端同为信道 1）；
  对端是否已上电；固定对端模式是否填了真实/伪装 MAC；自动发现模式需要对端
  也发帧（同固件板 / espnow-verify 按 `p0`）。
- **`espnow=PARTIAL`**：对端是“只听不回”的设备（如枪端正式固件），属预期；
  换成 espnow-verify 或另一块本固件板即可得到双向 PASS。
- **为什么不显示 RSSI？**：本固件以 **PlatformIO 官方环境兼容优先**
  （espressif32 6.x / Arduino core 2.0.17，与仓库其它固件一致）。该 core 的
  ESP-NOW 接收回调不带 RSSI；且官方 espressif32 **7.x 平台仍捆绑 core 2.0.17**
  （IDF 4.4），要拿 RSSI 需 Arduino core 3.x（IDF 5），而 core 3.x 官方
  PlatformIO 并未提供（仅社区分支支持，会牵连仓库其它工程）。故连接结论用
  **TX 链路层 ACK + 收帧计数**判定，足以区分 PASS/PARTIAL/FAIL；RSSI 留作可选增强。
