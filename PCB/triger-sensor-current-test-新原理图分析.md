# triger-sensor-current-test 新原理图分析（2026-08-20 用户更新版）

> ⚠️ **实验/测试主板**：电流变化前期研究用，后期实际 PCB 将重新设计。
> 数据来源：`PCB/triger-sensor-current-test_2026-08-20.epro2`（用户导出）+ 修正版
> `PCB/triger-sensor-current-test_fixed/triger-sensor-current-test_2026-08-20.fixed.epro2`。

## 1. 本次电路更新要点

| 变化 | 旧版（8-19） | 新版（8-20） |
| --- | --- | --- |
| 输入电源 | 5V（XT30） | **+11.1V（3S 电池）**，AMS1117-5.0 降压到 5V |
| MOSFET 断电开关 | 无 | **Q1（20N03 N-MOS）+ U4（UCC27517 栅极驱动）** |
| MOSFET 控制引脚 | — | **ESP32 G10 → R5(220Ω) → UCC27517 IN+ → OUT → R4(220Ω 缓冲) → Q1 栅极** |
| 状态 LED | 无 | **LED1（0805 红光）+ R3(220Ω)**，由 **G02** 驱动 |
| 采样电阻 | 10mΩ（RE2512F3R010） | **2mΩ（HoLLR2512-3W-2mR-1%，C2994640）** ← 本次更新 |
| ESP32 GND | 悬空 | pin20/21 已接地 |
| I2C（SDA/SCL） | G17/G18 | G17/G18（不变） |
| 电机/电池连接 | — | **由 XT30（CN1/CN2）外部连接闭合回路**（CN1 接电机侧、CN2 接电池侧） |

## 2. BOM（11 个实体元件）

| 位号 | 器件 | 料号 | 封装 | 说明 |
| --- | --- | --- | --- | --- |
| U1 | LCKFB-ESP32S3R8N8 | C20626143 | — | ESP32-S3 开发板 |
| U2 | AMS1117-5.0 | C6187 | SOT-223 | 11.1V→5V LDO |
| U3 | INA226AIDGSR | C49851 | VSSOP-10 | 电流/功率监视器（地址 0x40） |
| U4 | UCC27517DBVR(UMW) | — | SOT-23-5 | 4A 栅极驱动 |
| Q1 | 20N03 | C19271469 | — | N-MOS 20A，低端开关 |
| LED1 | FC-2012HRK-620D | — | 0805 | 红光 LED |
| R1 | **HoLLR2512-3W-2mR-1%（2mΩ 3W）** | **C2994640** | 2512 | 采样电阻（已从 10mΩ 更新） |
| R1(冲突) | 0603WAF2201T5E（2.2kΩ） | C4190 | 0603 | **SCL 上拉——与采样电阻位号重复，建议改 R6** |
| R2 | 0603WAF2201T5E（2.2kΩ） | C4190 | 0603 | SDA 上拉 |
| R3 | 0805W8F2200T5E（220Ω） | — | 0805 | LED 限流 |
| R4 | 0805W8F2200T5E（220Ω） | — | 0805 | **UCC27517.OUT ↔ Q1 栅极缓冲电阻**（串联阻尼） |
| R5 | 0805W8F2200T5E（220Ω） | — | 0805 | G10 → UCC27517.IN+ 限流 |
| C1/C2/C3 | CL31A106KBHNNNE（10uF） | C13585 | 1206 | 电源滤波 |

## 3. 网表要点（与 ESP32 相关）

- **I2C**：SDA=G17，SCL=G18 ✅
- **LED**：G02 → R3 → LED1(+) → GND（PIN_LED=2 一致）✅
- **MOSFET 控制**：G10 → R5 → UCC27517.IN+（IN- 接 GND）→ OUT → **R4（220Ω 缓冲）** → Q1.G（PIN_MOSFET=10）
- **采样回路**：+11.1V → R1(2mΩ) → NET2（Q1.D / U3.VIN-/VBUS）→ Q1.S → CN1.1（电机+）；电机/电池回路经 XT30 外部闭合
- 电源：U1 5V=pin24、3.3V=pin17、GND=pin20/21

## 4. 固件改动（本次已更新 firmware/trigger-board）

1. `config.h`：`PIN_MOSFET 15 → 10`（G10 经 UCC27517 驱动 20N03）
2. `config.h`：`SHUNT_MOHM 10 → 2`（采样电阻 2mΩ，C2994640）
3. `PowerSwitch.h` / `CurrentSense.cpp` 注释同步
4. 其余不变：`PIN_SDA 17 / PIN_SCL 18 / PIN_LED 2 / INA226_ADDR 0x40`（INA226 Config=0x06C7 → 1.18ms 转换周期）

> **2mΩ 采样电阻的影响**：INA226 分流 LSB 2.5µV → 分辨率 1.25mA；1A 触发阈值对应 2mV；
> 堵转 20A 时压降 40mV、损耗 0.8W（3W 电阻余量充足）；满量程 ±81.92mV ≈ 41A。

## 5. 原理图遗留问题（不影响固件运行）

1. **位号冲突**：两个 R1（2mΩ 采样电阻 和 2.2kΩ SCL 上拉），建议 SCL 上拉改 R6
2. ~~R4 被短路~~ → **已修正**：删除了 UCC27517.OUT→Q1.G 的旁路直连线，R4 现正确串联作为缓冲电阻
3. U1 电源引脚未全并联（5V/3.3V/GND 各只接部分；开发板内部连通，功能可用）
4. U4.IN+ 与 CN2.3（XT30 插接检测脚）同网络，确认设计意图
