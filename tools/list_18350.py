# -*- coding: utf-8 -*-
"""List 18350-power board components from gun-board epro2."""
import sys, collections
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type

entries = load_epru(r"PCB\gun-board_backup\gun-board_2026-08-22-16-15.epro2")
docs = docs_by_type(entries)
for sheet, label in (("6755c184abe52302", "18350-power P1"), ("4ea4588f82a06505", "main P1")):
    objs = docs["SCH_PAGE"][sheet]
    attrs = collections.defaultdict(dict)
    for c, pls in objs:
        if c.get("type") == "ATTR":
            for p in pls:
                if isinstance(p, dict) and p.get("parentId") and p.get("key") is not None:
                    attrs[p["parentId"]][p["key"]] = p.get("value")
    print(f"=== {label} components ===")
    for c, pls in objs:
        if c.get("type") != "COMPONENT":
            continue
        a = attrs.get(c.get("id"), {})
        des = a.get("Designator")
        nm = a.get("Name")
        dev = a.get("Device")
        if des or (nm and not des):
            print(f"  {str(des):<8} name={str(nm)[:40]:<40} dev={str(dev)[:16]}")
    print()
