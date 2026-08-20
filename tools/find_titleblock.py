"""Find title-block attribute storage in the schematic."""
import sys
import collections
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type
import json

entries = load_epru(r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test_2026-08-19-18-40.fixed.epro2")
docs = docs_by_type(entries)
objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]

comp_ids = {}
for c, pls in objs:
    if c.get("type") == "COMPONENT":
        for p in pls:
            if isinstance(p, dict):
                comp_ids[c.get("id")] = p.get("partId")

for c, pls in objs:
    if c.get("type") != "ATTR":
        continue
    for p in pls:
        if not isinstance(p, dict):
            continue
        if p.get("key") in ("@Page Name", "@Project Name", "@Schematic Name",
                            "Description", "Part Number", "Name", "Company"):
            pid = p.get("parentId")
            print(f"key={p.get('key')!r:22} value={p.get('value')!r:34} parent={pid} (comp={comp_ids.get(pid)})")
