"""Dump WIRE objects, and connectivity around U2/L1/D1/D2/C4 in the triger-sensor schematic."""
import os
import sys
import json
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_full_report import load_epru, docs_by_type

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.environ.get("TEMP", "."), "ts_bin", "triger-sensor_2026-08-31-11-37.epro2")

entries = load_epru(SRC)
docs = docs_by_type(entries)
objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]

print("=== WIRE objects (raw payload) ===")
for c, pls in objs:
    if c.get("type") != "WIRE":
        continue
    print("WIRE id=", c.get("id"))
    for pp in pls:
        s = json.dumps(pp, ensure_ascii=False)
        print("   ", s[:300])
