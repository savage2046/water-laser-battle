---
name: eda-offline-reader
description: >-
  离线读取嘉立创 EDA 专业版文件数据：.eprj2(SQLite)、.epro2/.epru(ZIP+行流) 中的
  元件/引脚/连线/网络/BOM。EDA 未运行或读历史备份时使用；触发词：嘉立创/立创EDA/
  EasyEDA 文件解析、原理图/PCB 数据、BOM、网表、.eprj2、.epro2、.epru。
license: MIT (vendored 上游各自许可见各自目录)
---

# EDA 离线读取技能（嘉立创 EDA 专业版）

## 文件格式速览（已实测魔数）

| 文件 | 本质 | 内容 |
| --- | --- | --- |
| `.eprj2` | SQLite（magic `SQLite format 3`） | 当前工程；`history_data` 为加密文档流 |
| `.epro2` | ZIP（magic `PK\x03\x04`） | 工程备份；内含 `.epru` 行记录流 + `project2.json` + `IMAGE/*.webp` |
| `.epru` | 文本行流 | 每行 `{header}||{body}|`（DOCHEAD/SYMBOL/WIRE/DEVICE…） |

本仓库真实文件：`PCB/<board>/<board>.eprj2` 与 `PCB/<board>_backup/*.epro2`。

## 三条解析路线

### 1) 社区逆向解析器（最常用，纯 Python 标准库）

位置：`tools/vendor/easyeda-agent-skills/`（eggfly，MIT；README-本地使用.md 有中文说明）

```powershell
python tools\vendor\easyeda-agent-skills\tools\parse_epro2.py "<某备份>.epro2"          # 元件表+电源符号+带网络标签的线
python tools\vendor\easyeda-agent-skills\tools\parse_epro2.py "<某备份>.epro2" --pins   # 全部引脚坐标
python tools\vendor\easyeda-agent-skills\tools\parse_epro2.py "<某备份>.epro2" --netlist# 引脚-连线网表映射
python tools\vendor\easyeda-agent-skills\tools\parse_epro2.py "<某备份>.epro2" --json   # 机器可读 JSON
```

- 已实测：`triger-sensor_2026-09-05-11-21.epro2` → 29 元件/51 电源符号/56 线/54 符号，退出码 0。
- 规范细节：`tools/vendor/easyeda-agent-skills/spec/epro2-format.md`（记录类型/旋转/坐标）。

### 2) 官方格式规范（权威对照）

位置：`tools/vendor/easyeda-api-skill/format/`（project/schematic/pcb 三组 + `index.md` 官方完整
规范下载链接）。描述 V3「日志」式行格式（与 `.epru` 同源）：`DOCHEAD` 切文档、
`{type,id,ticket}||{键值体}|`、外层一致性/内层原子数据。字段级细节与 1) 互相参照。

### 3) 本仓库历史脚本

`tools/read_eprj2_plain.py`、`dump_eprj2.py`、`dump_epru.py`、`netlist.py`、
`extract_sch.py`、`extract_full_report.py` 等（无第三方依赖，直接 `python tools\xxx.py …`）。
各板历史解析结论：`PCB/*/***-解析报告.md`。

## 执行注意（本机）

- `python`/`git` 属外部程序，**默认沙箱会拒绝**：pwsh 调用需带
  `sandbox_permissions: danger-full-access` 提权重试（弹人工批准；只对当次生效）。
- 只读探测（ls/读文件/端口扫描）无需提权。

## 深挖入口

- 中文总览：`skills/README.md`、`tools/vendor/easyeda-agent-skills/README-本地使用.md`
- 若要**在线操作运行中的 EDA**：改用 `skills/eda-official-api/SKILL.md`。
