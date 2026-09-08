# 嘉立创 EDA 官方 API 接入（easyeda-api-skill）

> 面向「在线操作运行中的嘉立创 EDA 专业版」的官方路线；与离线解析路线互补。
> 官方技能仓库已 vendored：`tools/vendor/easyeda-api-skill/`（v1.1.28，用法见其
> `README-本地使用.md`）。上游：https://github.com/easyeda/easyeda-api-skill

## 架构

```
┌──────────────┐  HTTP/WS     ┌─────────────────┐  WebSocket   ┌─────────────┐
│  AI / Agent  │ ◄──────────► │ Bridge Server   │ ◄──────────► │ EasyEDA Pro │
│  (本仓库会话) │  49620-49629 │  scripts/       │  49620-49629 │ (run-api-   │
└──────────────┘              │  bridge-server  │              │  gateway    │
                              └─────────────────┘              │  扩展)      │
                                                               └─────────────┘
```

## 两条读取路线怎么选

| | 离线解析（本仓库现状） | 在线官方 API（本页） |
|---|---|---|
| 对象 | `.eprj2`（SQLite）、`.epro2`/`.epru` 备份文件 | **运行中** EDA 打开的当前工程 |
| 依赖 | 无（纯 Python/文件读取） | Node Bridge + EDA 运行 + run-api-gateway 扩展 |
| 适合 | 审计历史版本、无需开 EDA、CI 校验 | 读取当前设计、导出 BOM/网表、验证或修改 |
| 工具 | `tools/vendor/easyeda-agent-skills/`、`tools/*.py`、官方 `format/` 规范 | 本页步骤 + `references/` 的 `eda.*` API |

## 组件清单

- 官方技能包（已落地本仓库）：`tools/vendor/easyeda-api-skill/`
- 官方扩展：**run-api-gateway**（https://jlc-ext.com/item/oshwhub/run-api-gateway）
- 依赖：Node ≥18、嘉立创 EDA 专业版（支持扩展的版本）

## 联调 checklist

1. EDA 专业版**已打开**目标工程；安装 run-api-gateway 扩展并允许外部交互。
2. 依赖与启动 Bridge（后台）：
   ```powershell
   cd tools\vendor\easyeda-api-skill
   npm install
   npm run server        # 自动占用 49620-49629 首个空闲端口；勿前台运行
   ```
3. 验证 EDA 已连：
   ```bash
   curl http://localhost:49620/health
   curl http://localhost:49620/eda-windows   # count>=1 且 active 窗口存在
   ```
   `count: 0` → 扩展没装上/没允许外部交互/EDA 未开，按 SKILL.md 提示处理。
4. 执行示例（先只读）：
   ```bash
   curl -X POST http://localhost:49620/execute -H "Content-Type: application/json" \
     -d '{"code":"return await eda.dmt_Project.getCurrentProjectInfo();"}'
   ```
   更丰富的读法（网表/元件/BOM）以 `tools/vendor/easyeda-api-skill/SKILL.md`
   与 `references/` 为准。

## 注意

- `execute` 在 EDA 进程内以 `AsyncFunction` 执行，拥有当前工程**完整权限**：先读后写、
  破坏性操作先人工确认。
- Bridge 端口固定 49620–49629 自选；占用冲突时改启动前检查。
- 官方 `format/` 规范描述的是 V3「日志」式源文件格式（与本仓库 `.epru` 行流同源），
  离线解析时与 eggfly 逆向规范互相参照。
