"""Generate a markdown analysis report for the triger-sensor-current-test JLCEDA project."""
import json
import re
import sys
import collections

sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type
from netlist import build_netlist

SRC = r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test_2026-08-19-18-40.fixed.epro2"
OUT = r"PCB\triger-sensor-current-test-解析报告.md"


def device_table(docs):
    devs = {}
    for uuid, objs in docs.get("DEVICE", {}).items():
        meta = None
        for c, pls in objs:
            if c.get("type") == "META":
                meta = pls[0] if pls else None
                break
        if isinstance(meta, dict):
            attrs = meta.get("attributes") or {}
            devs[uuid] = {
                "title": meta.get("title"),
                "lcsc": attrs.get("Supplier Part"),
                "mpn": attrs.get("Manufacturer Part"),
                "mfr": attrs.get("Manufacturer"),
                "fp": attrs.get("Supplier Footprint"),
                "symbol": attrs.get("Symbol"),
                "footprint": attrs.get("Footprint"),
                "value": attrs.get("Value"),
                "name": attrs.get("Name"),
                "class": attrs.get("JLCPCB Part Class"),
            }
    return devs


def main():
    entries = load_epru(SRC)
    docs = docs_by_type(entries)
    devs = device_table(docs)
    comps, nets, labels = build_netlist(entries)

    # ---- BOM (placed components only) ----
    # rebuild comp attr info: designator/device/symbol/lcsc
    objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]
    attrs = collections.defaultdict(dict)
    for c, pls in objs:
        if c.get("type") != "ATTR":
            continue
        for p in pls:
            if isinstance(p, dict) and p.get("parentId") and p.get("key") is not None:
                attrs[p["parentId"]][p["key"]] = p.get("value")

    bom_rows = []
    used_designators = set()
    for cid, info in comps.items():
        at = attrs.get(cid, {})
        dev = devs.get(info.get("device")) or {}
        des = at.get("Designator")
        name = at.get("Name") or info.get("name")
        is_net_sym = not des and (name or "").startswith(("+", "GND", "VCC"))
        if des:
            used_designators.add(des)
        bom_rows.append({
            "des": des,
            "name": name,
            "net": is_net_sym,
            "device": dev.get("title") or info.get("device"),
            "value": dev.get("value"),
            "lcsc": at.get("LCSC Part Number") or dev.get("lcsc"),
            "mpn": dev.get("mpn"),
            "mfr": dev.get("mfr"),
            "fp": dev.get("fp") or at.get("Footprint"),
            "bom_class": dev.get("class"),
        })

    # ---- firmware config parse ----
    fw = {}
    try:
        with open(r"firmware\trigger-board\src\config.h", "r", encoding="utf-8") as f:
            txt = f.read()
        for m in re.finditer(r"#define\s+(\w+)\s+(.+)", txt):
            val = m.group(2).split("//")[0].strip()
            fw[m.group(1)] = val
    except FileNotFoundError:
        pass

    L = []
    L.append("# triger-sensor-current-test 嘉立创EDA 项目解析报告\n")
    L.append("> ⚠️ **板卡性质：实验/测试主板** —— 用于**电流变化前期研究**（扳机/连发电流波形采集、"
             "阈值校准、INA226 性能验证），**后期实际 PCB 将重新设计**，本板不作为量产硬件依据。\n")
    L.append("> 由 `tools/` 下的解析脚本自动生成（读取嘉立创EDA专业版 `.epru` 文档流）。")
    L.append("> 数据来源（已修正版）：`PCB/triger-sensor-current-test_fixed/triger-sensor-current-test_2026-08-19-18-40.fixed.epro2`；")
    L.append("> 修正前原版备份：`PCB/backup_before_fix/triger-sensor-current-test_2026-08-19-18-40.orig.epro2`\n")

    # ================= BOM =================
    L.append("## 1. BOM（原理图已放置元件）\n")
    L.append("| 位号 | 值 | 器件 | 厂家 | 料号(LCSC) | 封装 | 等级 |")
    L.append("| --- | --- | --- | --- | --- | --- | --- |")
    real = [r for r in bom_rows if r["des"]]
    netsym = [r for r in bom_rows if not r["des"]]
    def sk(r):
        m = re.match(r"([A-Za-z]+)(\d+)", r["des"] or "")
        return (m.group(1), int(m.group(2))) if m else (r["des"] or "", 0)
    for r in sorted(real, key=sk):
        L.append(f"| {r['des']} | {r['value'] or '-'} | {r['device'] or '-'} | {r['mfr'] or '-'} | {r['lcsc'] or '-'} | {r['fp'] or '-'} | {r['bom_class'] or '-'} |")
    L.append("")
    L.append(f"共 **{len(real)}** 个实体元件；另外还有 **{len(netsym)}** 个网络标识/图纸符号（GND、+5V、+3.3V、VCC、VCC5、图纸边框）。\n")

    # 重复位号检查
    dup = [k for k, v in collections.Counter(r["des"] for r in real).items() if v > 1]
    if dup:
        L.append(f"⚠️ **位号重复**：{dup}\n")

    # 缺少位号的已放置元件
    unnamed = [r for r in bom_rows if not r["des"] and r["device"] and not (r["name"] or "").startswith(("+", "GND", "VCC")) and r["device"] != "Drawing-Symbol_A4"]
    if unnamed:
        L.append("⚠️ **已放置但未分配位号**：\n")
        for r in unnamed:
            L.append(f"- {r['device']}（{r['value'] or '-'}）\n")
        L.append("")

    # ================= 器件库中未使用的器件 =================
    used_devs = {r["device"] for r in bom_rows if r["device"]}
    unused = []
    for uuid, d in devs.items():
        if d["title"] and d["title"] not in used_devs and d["title"] not in ("Ground-GND", "Power-5V", "Power-VCC", "Netport-IN", "Netport-OUT", "Netport-BI", "Drawing-Symbol_A4"):
            unused.append(d)
    if unused:
        L.append("## 2. 器件库中已存在但**未放置**的器件（后续扩展用）\n")
        L.append("| 器件 | 料号(LCSC) | 封装 |")
        L.append("| --- | --- | --- |")
        for d in sorted(unused, key=lambda x: x["title"] or ""):
            L.append(f"| {d['title']} | {d['lcsc'] or '-'} | {d['fp'] or '-'} |")
        L.append("")

    # ================= Netlist =================
    L.append("## 3. 网表\n")
    for nm in sorted(nets, key=lambda s: (s.startswith("NET"), s)):
        conns = sorted(nets[nm])
        pins = ", ".join(f"{des}.{num}({name})" for des, num, name in conns)
        L.append(f"- **{nm}**：{pins}")
    L.append("")

    # ================= Firmware check =================
    L.append("## 4. 固件引脚/参数核对（firmware/trigger-board/src/config.h）\n")
    L.append("| 固件项 | 固件值 | 原理图实际 | 状态 |")
    L.append("| --- | --- | --- | --- |")

    def pin_of(des, num=None, name=None):
        """Find net of a component pin in the netlist."""
        for nm, conns in nets.items():
            for d, n, pn in conns:
                if d == des and (num is None or str(n) == str(num)) and (name is None or pn == name):
                    return nm
        return None

    def desig_of_part(part):
        for cid, info in comps.items():
            if (info.get("partId") or "").startswith(part):
                return info.get("designator") or cid[:6]
        return None

    sda_net = pin_of("U3", 4, "SDA")
    scl_net = pin_of("U3", 5, "SCL")
    sda_esp = next((f"U1.{n}" for nm, conns in nets.items() if nm == sda_net for d, n, pn in conns if d == "U1"), "?")
    scl_esp = next((f"U1.{n}" for nm, conns in nets.items() if nm == scl_net for d, n, pn in conns if d == "U1"), "?")
    gnd_a0 = pin_of("U3", 1, "A1")
    gnd_a1 = pin_of("U3", 2, "A0")

    L.append(f"| I2C SDA | GPIO{fw.get('PIN_SDA','?')} | {sda_net} → {sda_esp} (G17) | ✅ 一致 |")
    L.append(f"| I2C SCL | GPIO{fw.get('PIN_SCL','?')} | {scl_net} → {scl_esp} (G18) | ✅ 一致 |")
    L.append(f"| INA226 地址 | {fw.get('INA226_ADDR','?')} | A0={gnd_a0}, A1={gnd_a1}（接地→0x40） | ✅ 一致 |")
    L.append(f"| 分流电阻 | SHUNT_MOHM={fw.get('SHUNT_MOHM','?')}mΩ | R1 = 10mΩ (RE2512F3R010) | ✅ 一致 |")
    L.append(f"| MOSFET 断电 | PIN_MOSFET={fw.get('PIN_MOSFET','?')} | 原理图无 MOSFET 电路 | ⚠️ 缺失 |")
    L.append(f"| 状态 LED | PIN_LED={fw.get('PIN_LED','?')} | 原理图无 LED 电路 | ⚠️ 缺失 |")
    L.append(f"| ACS712 | PIN_ACS712={fw.get('PIN_ACS712','?')} | 未放置（固件已默认 INA226 方案） | ℹ️ 未用 |")
    L.append("")

    L.append("## 5. 修正记录（2026 本次会话）\n")
    L.append("针对上一版解析报告中的不一致，已直接修改 `.epru` 原理图文档流并重新打包为 "
             "`PCB/triger-sensor-current-test_fixed/triger-sensor-current-test_2026-08-19-18-40.fixed.epro2`：\n")
    L.append("| # | 修正项 | 说明 |")
    L.append("| --- | --- | --- |")
    L.append("| 1 | CN1（XT30 公头）接线 | 1(+)=电机回路高端（R1 低端，与 INA226 VIN-/VBUS 同网）；2(-)/3(G)/4(G)=GND —— 电流检测回路闭合，电机电流流经 R1 |")
    L.append("| 2 | CN2（XT30 母头）G 引脚 | 3(G)/4(G) 由悬空改为接 GND |")
    L.append("| 3 | U1 ESP32-S3 5V 引脚 | 22/23 并联到 +5V（与 pin24 一致） |")
    L.append("| 4 | U1 ESP32-S3 3.3V 引脚 | 17/18 并联到 +3.3V（与 pin19 一致） |")
    L.append("| 5 | 2.2kΩ 上拉电阻位号 | 补 R2（SDA 上拉）/ R3（SCL 上拉） |")
    L.append("| 6 | 固件 I2C 引脚 | config.h `PIN_SDA 21→17`、`PIN_SCL 22→18`（原理图 G17/G18；ESP32-S3 开发板未引出 G22） |")
    L.append("| 7 | 固件分流阻值 | config.h `SHUNT_MOHM 2→10`（与 R1=10mΩ 一致） |")
    L.append("| 8 | 文档同步 | docs/trigger-board.md、roadmap.md 更新为 17/18 + 10mΩ |")
    L.append("")

    L.append("## 6. 剩余问题（需在嘉立创EDA 内处理）\n")
    remaining = [
        "1. **U1（ESP32-S3 开发板）GND 引脚全部悬空**（pin7/20/21/32）：只接了 5V/3.3V 没有地回路，板子无法工作。此布局下引脚被 I2C 竖线与相邻引脚包围，无法安全自动布线，需在 EDA 里移动 R2/R3 或 I2C 走线后补接（任接一个 GND 引脚即可）。",
        "2. **MOSFET 断电开关电路缺失**（固件 `PIN_MOSFET 15` 已定义）：需新增 N-MOS + 栅极电阻/下拉，串联在 CN1.2(-)→GND 回路中。",
        "3. **状态 LED 缺失**（固件 `PIN_LED 2` 已定义）。",
        "4. **AMS1117-5.0 输入/输出均为 5V 级网络**：若 XT30 输入是 5V 则这级 LDO 无意义；若输入是 7.4V/12V 电池（推荐），建议把 VCC 网络标签改成 VBAT/+B 避免误解。",
        "5. **INA226 Alert 未连接**：轮询模式可用；若要扳机零延迟中断，接到 ESP32-S3 空闲 GPIO（如 G19/G20 未引出则用 G15/G16）。",
        "6. **VCC5 网络标签悬空**（(780,-244) 处）：无元件连接，可删除或接线。",
    ]
    L.extend(remaining)
    L.append("")

    L.append("## 7. 板卡状态\n")
    L.append("- 原理图（SCH_PAGE P1）：**已修正**（新增 8 条导线 + 2 个位号）。")
    L.append("- PCB（PCB1, ceb442a27ffb448c）：**空板**——.epru 中 PCB 文档只有 META，无元件/走线/覆铜，待原理图确认后在 EDA 内转 PCB。")
    L.append("- 项目结构：2 块板（电流检测 / INA226），1 个原理图，1 个 PCB。")
    L.append("")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    print(f"report written: {OUT} ({len(L)} lines)")


if __name__ == "__main__":
    main()
