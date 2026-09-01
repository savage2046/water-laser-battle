"""Dump LINE object payload shapes and any net attributes in the triger-sensor schematic."""
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

# keys used in LINE payloads
line_keys = collections.Counter()
line_groups = collections.Counter()
for c, pls in objs:
    if c.get("type") != "LINE":
        continue
    for pp in pls:
        if isinstance(pp, dict):
            line_keys.update(pp.keys())
            if pp.get("lineGroup"):
                line_groups[pp.get("lineGroup")] += 1

print("LINE payload keys:", dict(line_keys))
print("LINE groups:", len(line_groups))

# print a few full LINE payloads
n = 0
for c, pls in objs:
    if c.get("type") != "LINE":
        continue
    for pp in pls:
        if isinstance(pp, dict) and pp.get("lineGroup") and pp.get("startX") is not None:
            print("LINE:", json.dumps(pp, ensure_ascii=False)[:400])
            n += 1
            if n >= 15:
                break
    if n >= 15:
        break

# ELE_PLACEHOLDER payload
print("\n=== ELE_PLACEHOLDER sample ===")
n = 0
for c, pls in objs:
    if c.get("type") != "ELE_PLACEHOLDER":
        continue
    for pp in pls:
        if isinstance(pp, dict):
            print("PLACEHOLDER:", json.dumps(pp, ensure_ascii=False)[:400])
    n += 1
    if n >= 5:
        break
