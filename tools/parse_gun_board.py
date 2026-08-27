# -*- coding: utf-8 -*-
"""Parse gun-board .epro2 backup: list docs, dump main-board schematic BOM & PCB state."""
import json, sys, collections
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type, split_payload, deep_meta

PATH = r"PCB\gun-board_backup\gun-board_2026-08-22-16-15.epro2"
entries = load_epru(PATH)
docs = docs_by_type(entries)

print(f"total entries: {len(entries)}")
print("=== DOC types ===")
for t, d in docs.items():
    print(f"  {t}: {len(d)} docs -> {list(d.keys())[:10]}")

# ---- main board schematic: 099486cdadf0a3fa (ESP32主控), sheet 4ea4588f82a06505 ----
SCH = "099486cdadf0a3fa"
SHEET = "4ea4588f82a06505"
PCB = "419fae13055da83b"

print(f"\n=== SCH_PAGE {SHEET} objects ===")
objs = docs.get("SCH_PAGE", {}).get(SHEET, [])
print("obj count:", len(objs))
tcount = collections.Counter(c.get("type") for c, p in objs)
print("types:", dict(tcount))

attrs = collections.defaultdict(dict)
comps = []
for c, pls in objs:
    if c.get("type") == "COMPONENT":
        p = pls[0] if pls and isinstance(pls[0], dict) else {}
        comps.append((c.get("id"), p))
    elif c.get("type") == "ATTR":
        for p in pls:
            if isinstance(p, dict) and p.get("parentId") and p.get("key") is not None:
                attrs[p["parentId"]][p["key"]] = p.get("value")

print(f"\ncomponents on sheet: {len(comps)}")
for cid, p in comps:
    a = attrs.get(cid, {})
    des = a.get("Designator")
    dev = a.get("Device")
    name = a.get("Name")
    fp = a.get("Footprint")
    sym = a.get("Symbol")
    print(f"  {str(des):<8} dev={str(dev)[:40]:<40} name={str(name)[:28]:<28} fp={str(fp)[:18]} sym={str(sym)[:12]}")

# ---- main board PCB: 419fae13055da83b ----
print(f"\n=== PCB {PCB} objects ===")
pobjs = docs.get("PCB", {}).get(PCB, [])
print("obj count:", len(pobjs))
ptcount = collections.Counter(c.get("type") for c, p in pobjs)
print("types:", dict(ptcount))
for t in ("TRACK", "VIA", "COPPER", "POLY", "PAD", "COMPONENT", "NET", "RULE", "META", "TEXT"):
    n = ptcount.get(t, 0)
    if n:
        print(f"  {t}: {n}")

# PCB components
pattrs = collections.defaultdict(dict)
pcomps = []
for c, pls in pobjs:
    if c.get("type") == "COMPONENT":
        p = pls[0] if pls and isinstance(pls[0], dict) else {}
        pcomps.append((c.get("id"), p))
    elif c.get("type") == "ATTR":
        for p in pls:
            if isinstance(p, dict) and p.get("parentId") and p.get("key") is not None:
                pattrs[p["parentId"]][p["key"]] = p.get("value")
print(f"\nPCB components: {len(pcomps)}")
for cid, p in pcomps:
    a = pattrs.get(cid, {})
    print(f"  {str(a.get('Designator')):<8} {str(a.get('Name'))[:30]:<30} x={p.get('x')} y={p.get('y')} rot={p.get('rotation')}")

# META of PCB (board shape etc.)
for c, pls in pobjs:
    if c.get("type") == "META":
        print("\nPCB META:", json.dumps(pls[0] if pls else None, ensure_ascii=False)[:800])
