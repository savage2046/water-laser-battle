# easyeda-agent-skills —— 本地读取说明（中文）

> 本目录是 [eggfly/easyeda-agent-skills](https://github.com/eggfly/easyeda-agent-skills)
> 的 **vendored 副本**（MIT 许可，见 `LICENSE`），2026 由本仓库迁入、已去除 `.git`。
> 目标：让本仓库的 AI 会话与开发者能**离线读取嘉立创 EDA 专业版 `.epro2` / `.epru` 文件**，
> 不再每次从零逆向格式。

## 它读什么格式

嘉立创 EDA 专业版的备份文件 `.epro2` 本质是 **ZIP 压缩包**，内含：

```
├── project2.json            # 工程元信息
├── <ProjectName>.epru       # 主数据：逐行 {header}||{body}| 的文本记录流
└── IMAGE/*.webp             # 页面缩略图
```

`.epru` 每行是一条记录（`DOCHEAD / DEVICE / SYMBOL / WIRE / LINE / NETLABEL / PWR ...`），
可读、可 diff。**本仓库的真实用例**：`PCB/*_backup/*.epro2` 备份文件即此格式
（`.eprj2` 当前工程则是 SQLite，见下“局限”）。

## 目录内容

| 路径 | 作用 |
| --- | --- |
| `spec/epro2-format.md` | `.epru` 记录格式规范（记录类型/字段/JSON schema），366 行 |
| `spec/schematic-drawing.md` | 连线约定、电源符号、去耦电容套路 |
| `spec/coordinate-system.md` | 坐标系/旋转/标注位置规则 |
| `spec/api-reference.md` | LCSC/EasyEDA 官方 API 端点与数据格式 |
| `tools/parse_epro2.py` | **读**：解析元件位号/坐标/引脚/线/网络标签，支持 JSON/网表输出 |
| `tools/lcsc_fetch.py` | 联网：LCSC API 查料号、引脚、数据手册（urllib，无第三方依赖） |
| `tools/v6_to_epru.py` | 转换：EasyEDA v6 库数据 → 专业版 epru DEVICE/SYMBOL |
| `tools/gen_wires.py` / `build_schematic.py` | 写：从网表生成/增补连线与外围电路 |
| `tools/pack_epro2.py` | 打包回 `.epro2` ZIP |
| `examples/usage.md` | 上游完整走读示例 |

全部脚本**仅用 Python 标准库**（除 `lcsc_fetch`/`v6_to_epru` 需联网访问官方 API）。

## 快速上手（读本仓库真实文件）

```powershell
# 默认：元件位号/料号/坐标/旋转 + 电源符号表 + 带网络标签的线
python tools\vendor\easyeda-agent-skills\tools\parse_epro2.py `
  "PCB\triger-sensor\triger-sensor_backup\triger-sensor_2026-09-05-11-21.epro2"

# 其它模式
python ...\parse_epro2.py <文件.epro2> --pins      # 全部引脚坐标
python ...\parse_epro2.py <文件.epro2> --wires     # 已有连线
python ...\parse_epro2.py <文件.epro2> --json      # 机器可读 JSON
python ...\parse_epro2.py <文件.epro2> --netlist   # 引脚-连线 网表映射
```

**实测记录**（2026，`triger-sensor_2026-09-05-11-21.epro2`）：退出码 0，
解析出 `29 components / 51 power symbols / 56 wires / 54 symbols`，
元件表含位号、`HGC0603R5226M100NTHJ.1` 之类料号串、坐标与旋转。

## 局限与补充

- `parse_epro2.py` 面向**原理图文档**（SYMBOL/WIRE 等记录）。若需从文件读
  PCB 布局/布线、或解析 **`.eprj2`（SQLite，含加密 history 流）**，
  用本仓库既有脚本：`tools/read_eprj2_plain.py`、`dump_epru.py`、`netlist.py`、
  `extract_sch.py` 等；工程级 SQL 转储可参考第三方
  [neuroflag/easyeda-git](https://github.com/neuroflag/easyeda-git)。
- 文件里的旋转约定等细节以 `spec/` 为准；上游为逆向整理，版本升级后字段可能变动，
  遇到异常先看 `spec/epro2-format.md` 再比对记录。
- **官方对照**：嘉立创官方格式规范已 vendored 于
  `tools/vendor/easyeda-api-skill/format/`（V3 日志式行格式总纲 + 官方完整规范下载链接），
  解析时两者互相参照；在线读写运行中的 EDA 用官方 API（见 `docs/easyeda-official-api.md`）。
