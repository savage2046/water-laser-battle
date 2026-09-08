---
name: eda-official-api
description: >-
  通过嘉立创 EDA 官方 eda.* API 在线读取/操作运行中的 EDA 专业版（当前打开的工程）：
  工程结构、原理图/PCB 文档、元件、导出 BOM/网表、执行 eda.* 代码。需 EDA 运行 +
  run-api-gateway 扩展 + 本地 Bridge(49620-49629)。触发词：嘉立创EDA 启动/联调、
  eda.dmt_Project、在线读原理图/PCB、官方API。
---

# 嘉立创 EDA 官方 API 在线技能

架构：`AI/脚本 ──HTTP──► Bridge Server(49620-49629) ──WS──► EasyEDA Pro(run-api-gateway 扩展)`

## 前置检查（缺一不可）

1. 嘉立创 EDA **专业版正在运行**且打开目标工程；
2. 官方扩展 **run-api-gateway** 已安装并勾选「允许外部交互」
   （https://jlc-ext.com/item/oshwhub/run-api-gateway ，扩展广场搜 `oshwhub/run-api-gateway`）；
3. 本地 Bridge 在跑（或按下面启动）。

## 启动 / 复用 Bridge（本仓库 vendored 官方包）

```powershell
# 依赖（仅 ws，通常已装）
cd tools\vendor\easyeda-api-skill
npm install
# 后台启动（勿前台阻塞）
node scripts\bridge-server.mjs        # 自动占用 49620-49629 首个空闲端口
```

> 若此前会话已起过 Bridge（本机当前任务名 `pwsh-1`、端口 49620），先探测复用，勿重复起：
> `Invoke-RestMethod http://127.0.0.1:49620/health`

## 验证连接

```powershell
$h = Invoke-RestMethod "http://127.0.0.1:49620/health"      # edaConnected=true 才可执行
$w = Invoke-RestMethod "http://127.0.0.1:49620/eda-windows" # count≥1；多开需 POST /eda-windows/select
```

`edaConnected=false` → 扩展没加载/没开外部交互/EDA 未开；扩展可禁用再启用触发重连。

## 执行 `eda.*` 代码（读当前工程）

```powershell
$body = @{ code = 'return await eda.dmt_Project.getCurrentProjectInfo();' } | ConvertTo-Json
Invoke-RestMethod "http://127.0.0.1:49620/execute" -Method Post -ContentType 'application/json' -Body $body
```

实测返回结构示例（calibration-target）：`{itemType:Project, uuid, friendlyName, data:[
{Board1 → schematic{Schematic1, page[P1]} + pcb{PCB1}} , {Panel1}] }`。

## API 参考（离线可查）

- 快速索引：`tools/vendor/easyeda-api-skill/references/_quick-reference.md`（189KB）
- 总索引：`tools/vendor/easyeda-api-skill/references/_index.md`
- 类详情：`tools/vendor/easyeda-api-skill/references/classes/*.md`
  （`DMT_Project`/`DMT_Board`/`DMT_Schematic`、`SCH_*`/`PCB_*`/`LIB_*`/`SYS_*`）
- 完整技能指令：`tools/vendor/easyeda-api-skill/SKILL.md`（含读元件/网表/BOM/导出范式）
- 中文接入手册：`docs/easyeda-official-api.md`

## 安全与清理

- `execute` 以 `AsyncFunction` 跑在 EDA 进程内，拥有**当前工程完整权限**：先读后写，
  破坏性操作先给用户看代码再执行。
- Bridge 是常驻后台任务：不需要时 `job_kill`（本机为 `pwsh-1`）；EDA 关闭不影响 Bridge，
  重开 EDA 扩展会自动重连。
