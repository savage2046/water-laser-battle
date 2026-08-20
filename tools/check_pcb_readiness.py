"""Check PCB-conversion readiness of the current schematic:
footprints present, designators unique, net completion."""
import sys
import collections
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type

SRC = r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test_2026-08-20.fixed.epro2"
entries = load_epru(SRC)
docs = docs_by_type(entries)

objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]
attrs = collections.defaultdict(dict)
for c, pls in objs:
    if c.get("type") != "ATTR":
        continue
    for p in pls:
        if isinstance(p, dict) and p.get("parentId") and p.get("key") is not None:
            attrs[p["parentId"]][p["key"]] = p.get("value")

# footprint docs available
fp_uuids = set(docs.get("FOOTPRINT", {}).keys())
sym_uuids = set(docs.get("SYMBOL", {}).keys())

print("=== 元件清单（转PCB就绪检查）===")
real = []
for c, pls in objs:
    if c.get("type") != "COMPONENT":
        continue
    at = attrs.get(c.get("id"), {})
    des = at.get("Designator")
    sym = at.get("Symbol")
    fp = at.get("Footprint")
    if des:  # 实体元件
        real.append((des, fp, sym))
    # 网络标识无封装是正常的
    if des is None and (at.get("Name") or "").startswith(("+", "GND", "VCC", "NET")):
        pass

print(f"实体元件数: {len(real)}")
print(f"{'位号':<6} {'封装uuid':<18} {'封装库内?':<6} 备注")
dcount = collections.Counter()
for des, fp, sym in real:
    dcount[des] += 1
    fp_ok = "✅" if fp in fp_uuids else ("⚠️ 无/缺" if not fp else "⚠️ 库外")
    print(f"{des:<6} {str(fp):<18} {fp_ok:<8} ")

print("\n=== 位号重复 ===")
for des, n in dcount.items():
    if n > 1:
        print(f"  ⚠️ {des} 出现 {n} 次（转PCB会冲突）")

print("\n=== 元件封装库映射（FOOTPRINT 文档数）===")
print("FOOTPRINT docs:", len(fp_uuids), "| SYMBOL docs:", len(sym_uuids))

# 检查哪些组件 Footprint 缺失
print("\n=== 缺封装的元件 ===")
missing = [(d, s) for d, s, _ in real if s is None]
for d, s in missing:
    print(f"  ⚠️ {d}: footprint 属性为空")
if not missing:
    print("  无（全部有 Footprint 属性）")
