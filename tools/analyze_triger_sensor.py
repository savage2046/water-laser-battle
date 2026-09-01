"""Analyze the triger-sensor project (.epro2 backup or .epru) for doc updates."""
import os
import sys
import json
import collections

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools") if False else os.path.dirname(os.path.abspath(__file__)))
from extract_full_report import load_epru, docs_by_type

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.environ.get("TEMP", "."), "ts_bin", "triger-sensor_2026-08-31-11-37.epro2")

entries = load_epru(SRC)
docs = docs_by_type(entries)

print("=== DOC TYPES ===")
for dt, d in docs.items():
    print(f"  {dt}: {len(d)} docs")

print("\n=== SCH_PAGE object types ===")
objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]
cnt = collections.Counter(c.get("type") for c, pls in objs)
print(dict(cnt))

attrs = collections.defaultdict(dict)
for c, pls in objs:
    if c.get("type") != "ATTR":
        continue
    for pp in pls:
        if isinstance(pp, dict) and pp.get("parentId") and pp.get("key") is not None:
            attrs[pp["parentId"]][pp["key"]] = pp.get("value")

print("\n=== COMPONENTS (designator / name / device / pos) ===")
for c, pls in objs:
    if c.get("type") != "COMPONENT":
        continue
    cid = c.get("id")
    pos = {}
    for pp in pls:
        if isinstance(pp, dict):
            pos = {"x": pp.get("x"), "y": pp.get("y"), "rot": pp.get("rotation")}
    a = attrs.get(cid, {})
    des = a.get("Designator")
    name = a.get("Name")
    dev = str(a.get("Device"))[:12]
    print(f"  {str(des):8} | {str(name):16} | dev={dev:12} | pos={pos}")
