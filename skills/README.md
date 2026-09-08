# 技能 / 工具索引（供后续 AI 会话调用）

本目录把「读取/操作嘉立创 EDA 数据」的能力沉淀为可复用记录。**新会话处理 PCB/原理图相关任务时，
先读本文件与对应 SKILL，再动手。**

## 两个技能

| 技能 | 用途 | 什么时候用 |
| --- | --- | --- |
| [`eda-offline-reader`](eda-offline-reader/SKILL.md) | 离线解析 `.eprj2` / `.epro2` / `.epru` 文件（元件、引脚、连线、网表、BOM） | EDA 未运行 / 读历史备份 / 审计（多数场景） |
| [`eda-official-api`](eda-official-api/SKILL.md) | 在线调用嘉立创 EDA 官方 `eda.*` API（读/写运行中 EDA 的当前工程） | EDA 正在运行、需要读取当前设计或导出/修改 |

## 相关文件地图

| 路径 | 内容 |
| --- | --- |
| `tools/vendor/easyeda-agent-skills/` | 社区逆向解析器 + 格式规范（eggfly，MIT；`.epru` 行流字段细节），README-本地使用.md 为中文说明 |
| `tools/vendor/easyeda-api-skill/` | **嘉立创官方**技能（v1.1.28）：`references/` 120+ 类 `eda.*` API 文档、`format/` 官方源文件格式规范、`SKILL.md`、Bridge 脚本 |
| `tools/*.py` | 本仓库历史解析脚本（`read_eprj2_plain.py`、`dump_epru.py`、`netlist.py`、`extract_sch.py`、`dump_eprj2.py`…） |
| `PCB/README.md` | 板卡清单与数据源（哪个工程、哪个备份对应哪块板） |
| `PCB/*/***-解析报告.md` | 各板历史解析结论（BOM/网表/GPIO 映射） |
| `docs/easyeda-official-api.md` | 官方 API 接入手册（架构/checklist/安全） |
| `docs/trigger-board.md` 等 | 硬件设计文档（与固件对照） |

## 本机环境事实（重要，省得每次重踩）

- **DSH Desktop**（dataelement 版，harness `0.1.2-alpha.1`）；会话沙箱默认
  `workspace-write` + 审批 `ask`（每次弹人工确认）。
- **执行外部程序**（`git`/`python`/`node`/`npm`/`curl`）会被沙箱拒绝，需在 pwsh 调用上带
  `sandbox_permissions: danger-full-access` 提权重试（会弹一次人工批准；批准只对当次生效）。
  读取文件、目录浏览、端口/环境探测不受限。
- 用户级权限补丁（**不在本仓库**）：
  `%APPDATA%\dsh-desktop\harness\profiles\web\cordis.patch.yml`
  已把 `pwsh/bash/write` 设为 dsh-auto-review 的 `ai` 审查（生效后提权请求由审查子代理裁决，
  不再弹人工；排障时看该文件与 `/auto-review status`）。
- 网络：web 抓取/搜索、`Invoke-RestMethod` 可用；外部程序联网需先提权。
- 后台长任务：`pwsh` 的 `run_in_background: true` + 提权可起常驻服务（如 EDA Bridge）。

## 建议调用顺序

1. 判断场景 → 打开对应 SKILL.md；
2. 离线解析：直接用 `tools/vendor/easyeda-agent-skills/tools/parse_epro2.py` 或既有 `tools/*.py`；
3. 在线 API：确认 EDA 运行 + 扩展已连，`49620` 有 Bridge（见 eda-official-api SKILL），复用即可；
4. 结论写入对应 `PCB/*-解析报告.md` 或 `docs/`。
