# main 板 ESP32-S3 引脚映射（gun-board.eprj2 / 板子 main）——定稿

> 状态：**已定稿（2026-09-04）**，依据 gun-board 主控页原理图 P1 导出的
> `Netlist_ESP32主控_2026-09-04.enet` 与 `SCH_ESP32主控_2026-09-04.pdf`。
> **丝印约定已验证**：U1 排针丝印 `Gxx` = GPIOxx（DIP-40 表与 Protoflow KiCad
> 镜像逐脚一致，见 §6）。
>
> 标注：✅ = 原理图已连线（图确认，勿改）；🆕 = 空脚建议分配
> （EasyEDA 里按此接线后回填 ✅；固件已按本表写入）。

## 1. 功能 → S3 GPIO 映射表（固件 config.h 同值）

| 功能 | DIP 脚 | S3 GPIO | 标注 | 说明（主控页网络） |
| --- | --- | --- | --- | --- |
| IR_TX（940 远距 OOK） | 14 | **13** | ✅ | $1N92 → 220Ω → Q4 栅（H2 排针接 940 LED） |
| IR_TX_850（近距 940/56k） | 27 | **47** | ✅ | '850nm' → 200Ω → Q1 栅 |
| IR_PWR_850_A | 29 | **39** | ✅ | '850nm-2' → Q2 栅 |
| IR_PWR_850_B | 28 | **38** | ✅ | '850nm-3' → Q3 栅 |
| IR_RX（38k XL-IRM 接收） | 4 | **4** | 🆕 | 空脚；XL-IRM-V838M3/TR（C51900936） |
| IR_RX_850（56k 预留） | 5 | **5** | 🆕 | 空脚；56k 暂缓 |
| PIN_IR_POWER（940 功率档） | 15 | **14** | 🆕 | 空脚；940 单档硬件可不接 → config 置 0xFF |
| TRIGGER（扳机） | 6 | **6** | ✅ | G06 悬空 stub（主控页 $1N93），接微动开关 |
| MOTOR（水弹电机） | 8 | **7** | 🆕 | 空脚 |
| LED_DATA（WS2812） | 9 | **8** | 🆕 | 空脚 |
| I2S_BCLK / WS / DOUT | 9/12/11 所在脚 | **9 / 12 / 11** | 🆕 | 空脚（MAX98357A；WS 原 18 让给 OLED SCL） |
| OLED_SDA / SCL | 34 / 33 | **17 / 18** | ✅ | SDA=G17、SCL=G18 → OLED1 |
| LIGHT_SENSE（环境光 ADC） | 11 | **10** | 🆕 | 空脚；ADC1_CH10 |
| BTN_UP / DOWN / OK / CANCEL | 1/2/31/30 所在脚 | **1 / 2 / 21 / 40** | 🆕 | 空脚（内部上拉；GPIO0 保留板载 BOOT） |
| SX_NSS | 35 | **16** | ✅ | CS=G16 → U2.NSS |
| SX_SCLK | 38 | **42** | ✅ | SCK=G42 |
| SX_MOSI | 36 | **15** | ✅ | MOSI=G15 |
| SX_MISO | 37 | **41** | ✅ | MISO=G41 |
| SX_RST | 40 | **46** | ✅ | RaRES=G46（低有效） |
| SX_BUSY | 39 | **45** | ✅ | RaBUSY=G45 |
| SX_DIO1 | — | — | — | 不接（RadioLib 轮询） |
| 电源 | 17/24/20/21 | 3.3V/5V/GND | ✅ | +3.3V、+5V、GND×2 |

## 2. 已占用 GPIO 汇总（26 个）

`1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,45,46,47`

**仍然空闲可扩展**：G03、G48（GPIO3 为 strapping 建议避开；GPIO48=板载 LED 保留）。

## 3. S3 硬约束（与 §1 核对通过）

- GPIO26–37：Flash/Octal PSRAM 占用 → 全表未用 ✓
- GPIO19/20：USB → 未用 ✓；GPIO0：板载 BOOT → 未用 ✓
- ⚠️ GPIO45/46（SX BUSY/RST）：strapping 脚，主控页确认使用（立创板排针引出、上电时序无碍）

## 4. 待办

1. EasyEDA 主控页按 🆕 项接线（IR_RX=G04、IR_RX_850=G05、MOTOR=G07、LED=G08、
   I2S=G09/G11/G12、LIGHT=G10、IR_POWER=G14、BTN=G01/G02/G21/G40），完成后回填 ✅ 并重导 netlist 复核；
2. 若 940 通道为单档硬件（主控页 Q4/R447=47Ω 单档），把 `PIN_IR_POWER` 置 0xFF；
3. 其余板卡（头盔/校准靶/网关）仍为经典 ESP32 引脚方案，不受本表影响。

## 5. 备注

- 主控页 IR 驱动网络沿用历史命名 `850nm/-2/-3`（对应 Q1 OOK、Q2/Q3 功率档），
  波长已统一 940nm，网络名暂不改（PCB 侧无需动）。
- 原理图当前仅 P1（ESP32主控 页 1/1）；XL-IRM 接收头等后续功能页画入后更新本表。

## 6. 参考资料

- netlist：`PCB/gun-board/Netlist_ESP32主控_2026-09-04.enet`
- 原理图：`PCB/gun-board/SCH_ESP32主控_2026-09-04.pdf`（页 1/1）
- DIP-40 引脚表交叉验证：[Protoflow LCKFB-ESP32S3R8N8 KiCad](https://www.protoflow.ai/library/parts/C20626143)
  、[立创商城 LCKFB-ESP32S3R8N8](https://item.szlcsc.com/22034693.html)（C20626143）
