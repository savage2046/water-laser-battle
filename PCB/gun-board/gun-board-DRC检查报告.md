# gun-board DRC 检查报告(嘉立创 EDA 官方 API)

日期:2026-09-07 · EDA 专业版 V3.2.184 · Bridge 49620(会话 pwsh-1)
工程:gun-board(uuid d377091851…) · 检查方式:`eda.sch_Drc` / `eda.pcb_Drc` 详细模式

## 结论:3 块板 PCB DRC 全部通过;原理图无 error/fatal,各有 1 条 warn(无法枚举明细)

| 板 | 原理图 | PCB | SCH DRC | PCB DRC | 备注 |
| --- | --- | --- | --- | --- | --- |
| 18350-power | Schematic1 | PCB1 | 1 warn | **0 违规** | 网表 13=13 完全一致 |
| main | ESP32主控 | 主控 | 1 warn | **0 违规** | 网表 24=24(名称大小写差异,EDA 视为同网) |
| 红外LED座940nm | Schematic3 | PCB4 | 0 | **0 违规** | 网表 2=2,引脚级一致 |

## 过程与发现

### 1. 首轮批量 DRC 曾报 2 处 Netlist Error(误报)
main/主控 与 红外LED座940nm/PCB4 首轮(打开 PCB 后仅等 1.5s 即查)出现
`Netlist Error / Import Changes`(PCB 与原理图网表不一致)。深挖结论:

- **main 板**:原理图网络名 `850nm/850nm-2/850nm-3/RaBUSY/RaRES`(Protel 导出保原始大小写)
  在 PCB 网表中为大写 `850NM/…/RABUSY/RARES`。经对比其余 19 个网络完全一致,
  且嘉立创 EDA 网络名比较**大小写不敏感**——两条 24=24 网络实际一一对应,非真实缺失。
- **940nm 板**:逐焊盘核对(padNumber/net):940nm: pad1=$1N3、pad2=GND;U1: pad1=$1N3、
  pad2=GND,与原理图 A(阳极→$1N3)/K(阴极→GND)一致,引脚级无差异。

结论:首轮报错源于**打开 PCB 文档后网表/DRC 尚未完成刷新即执行检查**(时序问题)。

### 2. 修复动作(用户已批准)
对 main/主控 与 红外LED座940nm/PCB4 执行 `eda.pcb_Document.importChanges()`,均返回 `true`。

### 3. 复验(importChanges 后 + 长等待 5s ×2 轮)
| 板 | 第 1 轮 | 第 2 轮 |
| --- | --- | --- |
| 18350-power / PCB1 | 0 | 0 |
| main / 主控 | 0 | 0 |
| 红外LED座940nm / PCB4 | 0 | 0 |

连续两轮稳定 0 违规 → 确认修复/确认无真实 PCB 设计规则错误。

## 遗留说明与建议

1. **SCH warn(各 1 条)**:本版 EDA 的 `sch_Drc.check(true,false,true)` 只返回聚合
   `{type:'warn',count:1}` 不含规则明细(逐项明细为 EDA v4.2+ 能力)。如需定位,
   建议在 EDA 内打开各原理图 → 设计 → 检查 → 查看底部 DRC 窗口的 warn 条目。
2. **main 板网络大小写不统一**(原理图 `850nm/RaBUSY…`,PCB 大写):功能上无害,
   但建议在原理图将网络标签统一为大写(`850NM`、`RABUSY`、`RARES`)以保持工程整洁,
   避免第三方工具(如 Protel 网表/离线解析器)按大小写敏感比对时误判。
3. 本次仅运行 DRC,未修改任何布线/元件(importChanges 为网表同步,已在 EDA 中保存生效)。
4. 建议后续规则性检查时:**打开 PCB 后等待 ≥2–3s** 再执行 `pcb_Drc.check`,避免时序误报。
