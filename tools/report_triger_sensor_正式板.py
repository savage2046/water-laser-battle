"""Generate PCB/triger-sensor-解析报告.md from the parsed backup data.

Data source: the readable .epro2 backup committed in git HEAD
(PCB/triger-sensor_backup/triger-sensor_2026-08-31-11-37.epro2).
If not present in the working tree, it is extracted from git automatically.
"""
import os
import subprocess
import sys
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_full_report import load_epru, docs_by_type
from netlist import build_netlist, build_symbol_pins, pin_tip_abs

BACKUP_NAME = "triger-sensor_2026-08-31-11-37.epro2"
BACKUP_REPO = f"PCB/triger-sensor_backup/{BACKUP_NAME}"

def get_backup():
    """Return a path to a readable .epro2 backup (extract from git if needed)."""
    candidates = [
        os.path.join(os.environ.get("TEMP", "."), "ts_bin", BACKUP_NAME),
        os.path.join(os.environ.get("TEMP", "."), "ts_backups", BACKUP_NAME),
        BACKUP_REPO,
    ]
    for p in candidates:
        if os.path.exists(p):
            return p
    # extract from git HEAD
    os.makedirs(os.path.join(os.environ.get("TEMP", "."), "ts_bin"), exist_ok=True)
    dst = os.path.join(os.environ.get("TEMP", "."), "ts_bin", BACKUP_NAME)
    try:
        data = subprocess.check_output(["git", "show", f"HEAD:{BACKUP_REPO}"])
        with open(dst, "wb") as f:
            f.write(data)
        return dst
    except Exception:
        raise SystemExit(f"backup not found: {BACKUP_REPO} (git)")

SRC = get_backup()
OUT = r"PCB\triger-sensor-解析报告.md"

entries = load_epru(SRC)
docs = docs_by_type(entries)
objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]

attrs = collections.defaultdict(dict)
for c, pls in objs:
    if c.get("type") != "ATTR":
        continue
    for pp in pls:
        if isinstance(pp, dict) and pp.get("parentId") and pp.get("key") is not None:
            attrs[pp["parentId"]][pp["key"]] = pp.get("value")

devs = {}
for uuid, dlist in docs.get("DEVICE", {}).items():
    for c, pls in dlist:
        if c.get("type") == "META" and pls:
            m = pls[0]
            if isinstance(m, dict):
                devs[uuid] = m

sym_pins = build_symbol_pins(docs)
comps, nets, labels = build_netlist(entries)

# BOM rows
bom = {}
for c, pls in objs:
    if c.get("type") != "COMPONENT":
        continue
    cid = c.get("id")
    a = attrs.get(cid, {})
    des = a.get("Designator")
    if not des:
        continue
    dev = devs.get(a.get("Device"), {}) or {}
    da = dev.get("attributes") or {}
    bom[des] = {
        "device": dev.get("title"),
        "value": da.get("Value") or a.get("Value"),
        "lcsc": da.get("Supplier Part") or a.get("LCSC Part Number"),
        "mfr": da.get("Manufacturer"),
        "fp": da.get("Supplier Footprint") or a.get("Footprint"),
        "cls": da.get("JLCPCB Part Class"),
    }

L = []
L.append("# triger-sensor 嘉立创EDA 项目解析报告（正式板）\n")
L.append("> ✅ **正式板**：无改装水弹枪联动检测板——电流检测扳机/连发 + MOSFET 物理断电\n"
         "> + ESP-NOW 连枪端主控。由实验板（triger-sensor-current-test）迭代重画，\n"
         "> 自带 USB-C（原生 USB 编程/供电）与独立电源（XL1509 5V + AMS1117 3.3V）。\n")
L.append("> 数据来源（可读备份）：`PCB/triger-sensor_backup/triger-sensor_2026-08-31-11-37.epro2`"
         "（git HEAD 保留）；\n")
L.append("> 工程文件：`PCB/triger-sensor.eprj2`（嘉立创EDA专业版，2026-09-01 保存，内容与上述备份一致）。\n")
L.append("> 由 `tools/` 解析脚本读取 `.epru` 文档流自动生成。\n")

# ============ 1. 板卡结构 ============
L.append("## 1. 板卡结构\n")
L.append("| 项 | 内容 |")
L.append("| --- | --- |")
L.append("| 板卡 | 电流采样触发（board 0949e4de03fd39c1） |")
L.append("| 原理图 | INA226（schematic a30c1169d5365697，sheet P1） |")
L.append("| PCB | 电流采样（a63391f5039f097a）——**已摆件/布线中**：26 元件、30 过孔、5 覆铜区、32 网络 |")
L.append("| 块符号 | INA226（docType 17） |")
L.append("")

# ============ 2. BOM ============
L.append("## 2. BOM（原理图已放置元件，共 %d 个实体元件）\n" % len(bom))
L.append("| 位号 | 值 | 器件 | 厂家 | 料号(LCSC) | 封装 | 等级 |")
L.append("| --- | --- | --- | --- | --- | --- | --- |")
def sk(des):
    import re
    m = re.match(r"([A-Za-z]+)(\d+)", des)
    return (m.group(1), int(m.group(2))) if m else (des, 0)
for des in sorted(bom, key=sk):
    b = bom[des]
    L.append(f"| {des} | {b['value'] or '-'} | {b['device'] or '-'} | {b['mfr'] or '-'} | {b['lcsc'] or '-'} | {b['fp'] or '-'} | {b['cls'] or '-'} |")
L.append("")

# ============ 3. GPIO 映射 ============
L.append("## 3. ESP32-C3-WROOM-02-N4（U1）引脚映射\n")
L.append("| 引脚 | 名称 | 网络 | 用途 |")
L.append("| --- | --- | --- | --- |")
u1_net = {}
for nm, conns in nets.items():
    for d, num, pname in conns:
        if d == "U1":
            u1_net[str(num)] = nm
pin_use = {
    "1": ("+3.3V", "模块供电"),
    "2": ("NET19", "EN（R4 2.2kΩ 上拉到 3.3V）"),
    "3": ("SDA", "I2C → INA226 SDA"),
    "4": ("SCL", "I2C → INA226 SCL"),
    "5": ("NET18", "IO6 预留"),
    "6": ("NET17", "IO7 预留"),
    "7": ("NET16", "IO8 预留"),
    "8": ("NET15", "IO9 预留"),
    "9": ("GND", "地"),
    "10": ("NET14", "IO10 预留"),
    "11": ("NET13", "RXD 预留"),
    "12": ("NET12", "TXD 预留"),
    "13": ("D-", "USB D-（IO18，经 D3 USBLC6 到 USB1）"),
    "14": ("D+", "USB D+（IO19，经 D3 USBLC6 到 USB1）"),
    "15": ("OUT", "IO3 → UCC27517.IN+（栅极驱动）"),
    "16": ("NET11", "IO2 预留"),
    "17": ("NET10", "IO1 预留"),
    "18": ("NET9", "IO0 → R6(220Ω) → LED1"),
    "19": ("NET8", "EP 地焊盘"),
}
for num in ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19"]:
    net = u1_net.get(num, "-")
    use = pin_use.get(num, ("-", "-"))[1]
    L.append(f"| {num} | {pin_use.get(num, ('?', '?'))[0]} | {net} | {use} |")
L.append("")

# ============ 4. 电源链 ============
L.append("## 4. 电源链\n")
L.append("```")
L.append("XT30（CN1/CN2，电池/motor 回路外部闭合）→ +11.1V")
L.append("  ├─ R1(2mΩ 2512 3W, JER2512F3R002) 串联电池回路 ← INA226 VIN+/VIN- 跨测")
L.append("  ├─ U2 XL1509-5.0E1（11.1V→5V；L1 68uH + D2 B5817WS + C1~C3 22uF）")
L.append("  └─ U4 UCC27517 VDD（栅极驱动供电，11.1V）")
L.append("+5v：XL1509 输出 + USB-C VBUS（经 D3 USBLC6）→ U7 AMS1117-3.3 → +3.3V（C5 22uF）")
L.append("+3.3V → U1.3V3 / U3.VS+ / R2/R3/R4 上拉")
L.append("```")
L.append("")
L.append("- **电流检测**：R1 = 2mΩ 3W 2512（JER2512F3R002，C46634447）；"
          "INA226 VIN+=+11.1V（R1.1 侧）、VIN-=NET2（R1.2 侧）、VBUS=+11.1V；"
          "A0/A1 接地 → 地址 0x40")
L.append("- **断电执行**：U1.IO3 → U4.IN+ → U4.OUT → Q1.G(20N03)（直连，NET21）；"
          "HIGH=枪回路导通；断开即物理断电（防作弊硬保证）。"
          "注：R6(220Ω) 是 LED 限流（NET9/NET20），不在栅极回路")
L.append("- **USB 编程/供电**：Type-C（USB1）D+/D- → D3（USBLC6-2SC6 ESD）→ IO19/IO18 原生 USB；"
          "CC1/CC2 经 R9/R10（5.1kΩ）下拉，免 CH340")
L.append("")

# ============ 5. 网表 ============
L.append("## 5. 网表（几何解析）\n")
for nm in sorted(nets, key=lambda s: (s.startswith("NET"), s)):
    conns = sorted(nets[nm])
    pins = ", ".join(f"{des}.{num}({name})" for des, num, name in conns)
    L.append(f"- **{nm}**：{pins}")
L.append("")
L.append("> 几何网表对跨越走线/无焊点连接可能漏连（如 U2 周边 +5v 网络），"
          "关键连接以 §3/§4 与原理图为准。")
L.append("")

# ============ 6. 遗留问题 ============
L.append("## 6. 遗留问题（需在嘉立创EDA 内确认/处理）\n")
rem = [
    "1. **U2（XL1509）周边 +5v 网络几何解析不完整**：L1/D2/C1~C3 与 +5v 的连接在几何网表中"
    "表现为部分 NET 编号（NET32=U2.2+ L1.2 + D2.A；+5v 仅含 D3.5/U7.3/USB1 VBUS）——"
    "需确认 buck 输出经 L1 到 +5v 的连线与覆铜，建议 EDA 内补焊点/检查 DRC。",
    "2. **D1（SS34）悬空**：已放置未接线（NET24/NET25 单脚）。若作为 XL1509 输入/输出保护"
    "或反接保护需接线；否则可删除或保留备用。",
    "3. **C4 悬空**（NET22/NET23 单脚）：已放置未接线，确认是否应为某电源去耦。",
    "4. **Q1 栅极直连**：网表显示 U4.OUT 直连 Q1.G（NET21 仅 2 脚），无栅极串联阻尼电阻。"
    "20N03 Qg≈10nC、UCC27517 4A 峰值驱动，直连可接受；若实测振铃明显可加 10~22Ω。",
    "5. **USB1 SBU1/SBU2/EH 悬空**（NET26/NET28/NET30/NET31）：Type-C 16P 的 SBU 与外壳脚"
    "未接，确认不需要音频/附件支持即可。",
    "6. **U1 大部分 GPIO 未接**（IO1/IO2/IO6~IO10/RXD/TXD/EP）：预留，符合设计。",
    "7. **INA226 Alert 未接**（U3.3 = NET7 单脚）：轮询模式可用；若要扳机零延迟中断，"
    "接到 ESP32-C3 空闲 GPIO（如 IO1/IO2）。",
]
L.extend(rem)
L.append("")

# ============ 7. 板卡状态 ============
L.append("## 7. 板卡状态\n")
L.append("- 原理图（SCH_PAGE P1）：**正式板完整重画**（ESP32-C3 模组 + XL1509 + AMS1117-3.3 + USB-C + INA226 + UCC27517 + 20N03）。")
L.append("- PCB（电流采样）：**已摆件/布线中**——26 元件、30 过孔、5 覆铜区、32 网络（.epru PCB 文档含 COMPONENT/VIA/POUR/NET/PAD_NET）。")
L.append("- 固件对齐：`firmware/trigger-board/src/config.h` `PIN_SDA 4/PIN_SCL 5/PIN_MOSFET 3/PIN_LED 0`、`SHUNT_MOHM 2`；`platformio.ini` 板卡 `esp32-c3-devkitm-1`。")
L.append("- 配套文档：[docs/trigger-board.md](../docs/trigger-board.md)、[docs/trigger-sensor-study.md](../docs/trigger-sensor-study.md)。")
L.append("")

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(L))
print(f"report written: {OUT} ({len(L)} lines)")
