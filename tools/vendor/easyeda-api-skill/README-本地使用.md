# easyeda-api-skill（嘉立创 EDA 官方 API 技能）本地说明

> 本目录是 [easyeda/easyeda-api-skill](https://github.com/easyeda/easyeda-api-skill)（嘉立创 EDA
> **官方组织**仓库）的 **vendored 副本**：`package.json` 版本 **1.1.28**（name: `easyeda-api`）。
> 许可：`SKILL.md` frontmatter 声明 **MIT**、作者 JLCEDA（仓库根目录无独立 LICENSE 文件，以
> frontmatter 与官方仓库页为准）。已去除 `.git`，总计约 3.1 MB。

## 它提供什么

1. **完整 `eda.*` API 参考**：`references/` 收录 **120+ classes / 62 enums / 70 interfaces /
   19 type aliases**（`DMT_Project`/`DMT_Board`/`DMT_Schematic`、`SCH_*`、`PCB_*`、`LIB_*`、
   `SYS_*`…），供 AI 与开发者离线查阅（`references/_quick-reference.md` 189KB 快速索引）。
2. **文档源码格式规范** `format/`：工程/原理图/PCB 源文件的**官方**格式说明（V3「日志」式
   逐行记录 `{type,id,ticket}||{键值体}|`，`DOCHEAD` 切分文档；`docType` 含 SCH/SCH_PAGE/
   PCB/SYMBOL/FOOTPRINT/DEVICE/BLOB/INSTANCE 等）。
3. **WebSocket Bridge** `scripts/bridge-server.mjs`：Node 服务，把 AI 的 HTTP 调用桥接进
   **正在运行**的嘉立创 EDA 专业版（端口 49620–49629，握手 `easyeda-bridge`）。
4. **SKILL.md**（746 行，AgentSkills 标准）：技能指令本身。
5. `guide/` 扩展开发指南；`user-guide/using-extension.md` 扩展使用说明。

## 与本仓库离线解析器的关系

| | eggfly `tools/vendor/easyeda-agent-skills` | 官方 `format/`（本目录） |
|---|---|---|
| 来源 | 社区逆向（面向 `.epro2` ZIP 内 `.epru` 行流） | **官方**（V2.2/V3 源文件格式总纲） |
| 内容 | 记录字段级实操细节（旋转、坐标换算等） | 权威框架：日志/一致性/`DOCHEAD`/类型约定 |
| 建议 | 解析 `.epru` 时按字段细节取用 | 对照校验、理解文档边界与删除语义 |

官方 `format/index.md` 还给出完整规范下载：V2.2 与 V3 的 zip/md/pdf
（https://image.lceda.cn/files/lceda-pro-file-format-v3_2025.10.21.md 等）。

## 用法一：纯离线参考（不启动 EDA）

把以下路径交给 AI/自己查阅即可：

- 想查某个 `eda.*` 类：`references/_index.md` → `references/classes/*.md`
- 想分析工程文件格式：`format/project/`、`format/schematic/`、`format/pcb/`
- 想开发 EDA 扩展：`guide/` + `references/` + `user-guide/`

## 用法二：在线 Bridge 联调（操作运行中的 EDA）

前置：Node ≥18；嘉立创 EDA 专业版运行中，且装好官方扩展
**run-api-gateway**（https://jlc-ext.com/item/oshwhub/run-api-gateway 或扩展广场搜
`oshwhub/run-api-gateway`），并在扩展管理中允许外部交互。

```powershell
# 1) 依赖（仅 ws）
npm install

# 2) 启动 Bridge（必须在后台跑，勿阻塞）
npm run server        # 自动占用 49620-49629 中首个空闲端口
```

EDA 端扩展会自动扫描端口并握手连接。之后：

```bash
# 健康检查（返回 {"service":"easyeda-bridge",...}）
curl http://localhost:49620/health

# 列出已连接的 EDA 窗口（0 个=扩展未连上；多开需选一个）
curl http://localhost:49620/eda-windows
curl -X POST http://localhost:49620/eda-windows/select -H "Content-Type: application/json" -d '{"windowId":"<id>"}'

# 在 EDA 内执行代码（完整 eda.* 面）
curl -X POST http://localhost:49620/execute -H "Content-Type: application/json" `
  -d '{"code":"return await eda.dmt_Project.getCurrentProjectInfo();"}'
```

> ⚠️ 安全提示：`execute` 的代码以 `new AsyncFunction` 跑在 EDA 进程内，拥有**当前工程的完整
> 权限**（可改图、可导出）。由 AI/脚本驱动时先只读查询，任何修改/删除前先人工确认。
