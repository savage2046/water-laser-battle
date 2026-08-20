# PCB 目录说明（triger-sensor-current-test 电流检测实验板）

> ⚠️ **实验/测试主板**：用于**电流变化前期研究**（扳机/连发电流波形采集、阈值校准、
> INA226 性能验证）。**后期实际 PCB 将重新设计**，本板不作为量产硬件依据。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `triger-sensor-current-test.eprj2` | 嘉立创EDA专业版项目文件（SQLite 格式，作者原版，仅改项目名为 triger-sensor-current-test） |
| `triger-sensor-current-test_backup/*.epro2` | 作者在嘉立创EDA里保存的工程备份（原版，改名） |
| `triger-sensor-current-test_fixed/triger-sensor-current-test_2026-08-19-18-40.fixed.epro2` | **修正后的工程备份**（在嘉立创EDA专业版中打开/导入此文件查看修正结果；原理图标题栏 Description 已注明"实验/测试主板"） |
| `backup_before_fix/*.orig.epro2`、`*.orig.eprj2` | 修正/改名前的原始备份（可回退） |
| `triger-sensor-current-test-解析报告.md` | 自动生成的解析报告：BOM、网表、固件核对、修正记录、剩余问题 |

## 板卡内容

- 原理图：INA226（10mΩ 分流电流检测）+ ESP32-S3 开发板（LCKFB-ESP32S3R8N8）
  + AMS1117-5.0 + XT30 对插（CN1 接电机回路 / CN2 接电池）
- PCB1：**空板**（尚未摆件布线，等实验方案确认后再设计）

## 已知修正（对比作者原版）

1. CN1（XT30 公头）由悬空改为接入电机回路（电流检测回路闭合）
2. CN2 G 引脚、U1 5V/3.3V 引脚并联、2.2kΩ 上拉补位号 R2/R3
3. 固件对齐：`PIN_SDA 17 / PIN_SCL 18`、`SHUNT_MOHM 10`、INA226 Config 修正为 `0x06C7`
   （修复 AVG=64 导致转换周期 46.6ms 的 bug → 1.18ms，满足 ≤10ms 电流检测）

## 2026-08-20 原理图更新（用户重画版 + 本次修正）

- `triger-sensor-current-test_2026-08-20.epro2`：用户新版原理图（输入 +11.1V 3S 电池、
  UCC27517 栅极驱动 + 20N03 MOSFET + LED + 220Ω 电阻 ×3 + C3）
- `triger-sensor-current-test_fixed/triger-sensor-current-test_2026-08-20.fixed.epro2`：**修正版**
  （采样电阻 10mΩ→**2mΩ**（HoLLR2512-3W-2mR-1%，C2994640）；删除旁路直连线使 R4 恢复为
  UCC27517→Q1 栅极间的 220Ω 缓冲电阻）
- 固件已同步：`PIN_MOSFET 15→10`、`SHUNT_MOHM 10→2`，LED 走 G02，I2C 走 G17/G18
- 电机/电池回路经 XT30（CN1/CN2）外部连接闭合
- 详情见 `triger-sensor-current-test-新原理图分析.md`

详见 `triger-sensor-current-test-解析报告.md` 与 `docs/trigger-board.md`。
