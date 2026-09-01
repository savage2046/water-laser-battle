"""Trace full wire groups and nearby pins for unresolved nets."""
import os
import sys
import json
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_full_report import load_epru, docs_by_type
from netlist import build_symbol_pins, pin_tip_abs

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.environ.get("TEMP", "."), "ts_bin", "triger-sensor_2026-08-31-11-37.epro2")

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

sym_pins = build_symbol_pins(docs)

comps = {}
for c, pls in objs:
    if c.get("type") != "COMPONENT":
        continue
    cid = c.get("id")
    pos = {}
    for pp in pls:
        if isinstance(pp, dict):
            pos = {"x": pp.get("x"), "y": pp.get("y"), "rot": pp.get("rotation")}
    a = attrs.get(cid, {})
    des = a.get("Designator") or a.get("Name")
    if des:
        comps[des] = {"id": cid, "pos": pos, "symbol": a.get("Symbol"),
                      "des": a.get("Designator"), "name": a.get("Name")}

wires = collections.defaultdict(list)
for c, pls in objs:
    if c.get("type") != "LINE":
        continue
    for pp in pls:
        if isinstance(pp, dict) and pp.get("lineGroup") and pp.get("startX") is not None:
            wires[pp["lineGroup"]].append(
                (pp["startX"], pp["startY"], pp["endX"], pp["endY"]))

# all pin tips
pin_tips_all = []
for des, info in comps.items():
    sym = info["symbol"]
    if sym not in sym_pins:
        continue
    for pid, pin in sym_pins[sym].items():
        node = pin_tip_abs(pin, info["pos"]["x"], info["pos"]["y"], info["pos"]["rot"])
        pin_tips_all.append((des, pin.get("number"), pin.get("name"), node))

def seg_contains(seg, x, y, tol=1.0):
    x1, y1, x2, y2 = seg
    if abs(x1 - x2) < 1e-6:
        return abs(x - x1) < tol and min(y1, y2) - tol <= y <= max(y1, y2) + tol
    if abs(y1 - y2) < 1e-6:
        return abs(y - y1) < tol and min(x1, x2) - tol <= x <= max(x1, x2) + tol
    return False

def trace_group(wid):
    """All segments in group + all pin tips touching them."""
    segs = wires[wid]
    hits = set()
    for (x1, y1, x2, y2) in segs:
        for des, num, name, node in pin_tips_all:
            if seg_contains((x1, y1, x2, y2), node[0], node[1]):
                hits.add((des, num, name, node))
    return segs, hits

for wid in ["bc449494a53e0558", "a560171cb9619503", "1c738a49d7e906c8",
            "a829efc6224280f2", "093029b66b6f034c", "1bb5cdcb52ea44d6",
            "25c0422a0117666d", "61c24bc2da41076b", "2ea6277f20e6b8a2",
            "2c4e0acaf5b2fa45", "55e93b8a9c9d2b91", "00d59e586d31c311",
            "f10e0b6d8be93eaa", "bec48caa25f34f65", "3b325f6cf91a1c29"]:
    segs, hits = trace_group(wid)
    print(f"=== wire {wid} ({len(segs)} segs)")
    for s in segs[:8]:
        print(f"    seg {s}")
    for h in sorted(hits):
        print(f"    PIN {h[0]} {h[1]} ({h[2]}) @ {h[3]}")
    print()
