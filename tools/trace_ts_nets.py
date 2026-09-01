"""Trace specific connections in triger-sensor schematic: LED/R6, R9/R10, U2 buck chain, D1/D2/L1, Q1."""
import os
import sys
import json
import collections
import math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_full_report import load_epru, docs_by_type
from netlist import build_symbol_pins, pin_tip_abs, rotate

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
    des = a.get("Designator")
    if not des:
        continue
    comps[des] = {"id": cid, "pos": pos, "symbol": a.get("Symbol"),
                  "name": a.get("Name"), "gnn": a.get("Global Net Name")}

# wires
wires = collections.defaultdict(list)
for c, pls in objs:
    if c.get("type") != "LINE":
        continue
    for pp in pls:
        if isinstance(pp, dict) and pp.get("lineGroup") and pp.get("startX") is not None:
            wires[pp["lineGroup"]].append(
                (pp["startX"], pp["startY"], pp["endX"], pp["endY"]))

def pin_tips(des):
    if des not in comps:
        return {}
    info = comps[des]
    sym = info["symbol"]
    if sym not in sym_pins:
        return {}
    out = {}
    for pid, pin in sym_pins[sym].items():
        node = pin_tip_abs(pin, info["pos"]["x"], info["pos"]["y"], info["pos"]["rot"])
        out[pid] = (pin.get("number"), pin.get("name"), node)
    return out

def seg_contains(seg, x, y, tol=1.0):
    x1, y1, x2, y2 = seg
    # check point on segment (axis-aligned mostly)
    if abs(x1 - x2) < 1e-6:  # vertical
        return abs(x - x1) < tol and min(y1, y2) - tol <= y <= max(y1, y2) + tol
    if abs(y1 - y2) < 1e-6:  # horizontal
        return abs(y - y1) < tol and min(x1, x2) - tol <= x <= max(x1, x2) + tol
    return False

def wires_touching(x, y, tol=1.0):
    out = []
    for wid, segs in wires.items():
        for s in segs:
            if seg_contains(s, x, y, tol):
                out.append((wid, s))
    return out

def net_of_point(x, y, tol=1.0):
    """Union-find wires touching this point, then find any label on those wires."""
    touched = wires_touching(x, y, tol)
    if not touched:
        return None
    # gather all segments of those wire groups
    groups = set(w for w, s in touched)
    segs = []
    for g in groups:
        segs.extend(wires[g])
    # find component pin tips on these segments (labels have no designator)
    hits = []
    for des, info in comps.items():
        for pid, (num, name, node) in pin_tips(des).items():
            for s in segs:
                if seg_contains(s, node[0], node[1], tol):
                    hits.append((des, num, name, node))
    return groups, segs, hits

print("=== NET20 trace: LED1 / R6 ===")
for des in ["LED1", "R6"]:
    print(f"--- {des} tips:", pin_tips(des))
print("wires touching LED1 pin2(+):", wires_touching(*pin_tips("LED1")[list(pin_tips("LED1"))[0]][2]))
for pid, (num, name, node) in pin_tips("R6").items():
    print(f"R6 pin {num} ({name}) tip {node} wires:", wires_touching(node[0], node[1]))

print()
print("=== R9 / R10 (5.1k) ===")
for des in ["R9", "R10"]:
    for pid, (num, name, node) in pin_tips(des).items():
        print(f"{des} pin {num} tip {node} wires:", wires_touching(node[0], node[1]))

print()
print("=== U2 XL1509 / L1 / D1 / D2 / C1..C5 / U7 ===")
for des in ["U2", "L1", "D1", "D2", "U7"]:
    print(f"--- {des}")
    for pid, (num, name, node) in pin_tips(des).items():
        print(f"   pin {num} ({name}) tip {node} wires:", wires_touching(node[0], node[1]))
