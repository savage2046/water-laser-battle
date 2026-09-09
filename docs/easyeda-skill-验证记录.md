# 嘉立创EDA Skill 功能验证记录

日期：2026-09-07（Bridge 会话 session-34eff4af）
环境：嘉立创EDA专业版 V3.2.184 运行中（gun-board 工程已打开）；DSH Desktop 0.8.0-rc.4
依据：`skills/eda-offline-reader/SKILL.md`、`skills/eda-official-api/SKILL.md`

## 1. 离线技能 eda-offline-reader —— 通过

### 1.1 `.epro2` 社区逆向解析器（无需提权即可运行）

```powershell
python tools\vendor\easyeda-agent-skills\tools\parse_epro2.py `
  "PCB\triger-sensor\triger-sensor_backup\triger-sensor_2026-09-05-11-21.epro2"
```

结果：`Parsed: 29 components, 51 power symbols, 56 wires, 54 symbols`，退出码 0，
与 SKILL 记录的历史实测一致。元件表（C1–C4、CN1/CN2、D1–D3、ESP32-C3-WROOM-02-N4、
XL1509、INA226、UCC27517、TYPE-C 等）与电源符号/网络标签全部解析正常。

### 1.2 `.eprj2`（SQLite）读取（需 danger-full-access 提权）

```powershell
python tools\dump_eprj2.py "D:\ESP32\water-laser-battle\PCB\calibration-target\calibration-target.eprj2"
```

结果：退出码 0，35 张表（branches/project_structures/history_data/components/schematics/…）
结构齐全；`history_data` 为 base64+加密文档流（与 SKILL 描述一致）。

> 注意点：
> - `check_eprj2.py` 硬编码 DB 路径 `PCB\triger-sensor-current-test.eprj2`，不接受 argv 参数，勿误用。
> - 当前被 EDA 打开的工程（如 gun-board.eprj2）文件被进程独占，离线读取会失败——属正常现象，
>   应改读其他工程或历史备份。

## 2. 在线技能 eda-official-api —— 通过（含重连）

### 2.1 Bridge 启动与连接

```powershell
cd tools\vendor\easyeda-api-skill
node scripts\bridge-server.mjs        # 后台任务 pwsh-1，端口 49620
```

- 首次 `/health` 探测曾出现 `edaConnected:false`（Bridge 启动瞬间扩展尚未连入）。
- 约 1–2 秒后 Bridge 日志显示 `[WS] EDA window registered: 2bdd5469-…`，随后心跳正常。
- 再次 `/health`：`edaConnected: true, edaWindowCount: 1, activeWindowId: 2bdd5469-…`；
  `/eda-windows`：1 个窗口 connected + active。→ **重连成功**

扩展确认为已安装：EDA 本地 IndexedDB（`%LOCALAPPDATA%\lceda-pro\cache.x64.3\IndexedDB`）
含 `run-api-gateway` / `oshwhub` / `49620` / `easyeda-bridge` 键。

### 2.2 `eda.*` 代码执行（端到端读取当前工程）

```javascript
return await eda.dmt_Project.getCurrentProjectInfo();
```

返回完整工程树（截取顶层）：

| 工程 | board | schematic | pcb |
| --- | --- | --- | --- |
| gun-board (uuid d377091851…) | 18350-power | Schematic1 (1页) | PCB1 |
|  | main | ESP32主控 (1页) | 主控 |
|  | 红外LED座940nm | Schematic3 (1页) | PCB4 |

- 中文板名/原理图名经 node fetch 验证为**完整 UTF-8**（PowerShell `Invoke-RestMethod`
  控制台显示乱码是本地解码问题，非 API 数据问题）。
- 全程只读，未做任何写入操作。

## 3. 结论与建议

1. 两个技能定义与其 vendored 工具在本机均可用，功能与 SKILL 文档描述一致。
2. Bridge（后台任务 pwsh-1，端口 49620）本次保留运行；不再需要时 `job_kill`。
3. 多 EDA 窗口场景可用 `POST /eda-windows/select` 切换活动窗口后再 execute。
4. 后续读取/操作 gun-board 工程时优先走 Bridge；EDA 关闭后离线技能仍可读历史备份。
