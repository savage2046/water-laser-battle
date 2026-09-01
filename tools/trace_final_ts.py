"""Final targeted trace: CN1 wires, Q1 source, NET3, U2 ON/OFF, U1 EN pin, D1/C4 status."""
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
                      "des": a.get("Designator"), "name": a.get("Name"),
                      "gnn": a.get("Global Net Name")}

wires = collections.defaultdict(list)
for c, pls in objs:
    if c.get("type") != "LINE":
        continue
    for pp in pls:
        if isinstance(pp, dict) and pp.get("lineGroup") and pp.get("startX") is not None:
            wires[pp["lineGroup"]].append(
                (pp["startX"], pp["startY"], pp["endX"], pp["endY"]))

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

def pins_on_point(x, y, tol=3.0):
    out = []
    for des, num, name, node in pin_tips_all:
        if abs(node[0] - x) < tol and abs(node[1] - y) < tol:
            out.append((des, num, name, node))
    return out

def wires_touching(x, y, tol=1.0):
    out = set()
    for wid, segs in wires.items():
        for s in segs:
            if seg_contains(s, x, y, tol):
                out.add(wid)
    return out

print("=== CN1 pins: nearby pins / wires ===")
for des in ["CN1"]:
    info = comps[des]
    sym = info["symbol"]
    for pid, pin in sym_pins[sym].items():
        node = pin_tip_abs(pin, info["pos"]["x"], info["pos"]["y"], info["pos"]["rot"])
        near = pins_on_point(node[0], node[1])
        wt = wires_touching(node[0], node[1])
        print(f"CN1 pin {pin.get('number')} ({pin.get('name')}) tip {node} wires={wt} near={[(n[0],n[1],n[2]) for n in near]}")

print()
print("=== Q1 all pins ===")
info = comps["Q1"]
for pid, pin in sym_pins[info["symbol"]].items():
    node = pin_tip_abs(pin, info["pos"]["x"], info["pos"]["y"], info["pos"]["rot"])
    near = pins_on_point(node[0], node[1])
    wt = wires_touching(node[0], node[1])
    print(f"Q1 pin {pin.get('number')} ({pin.get('name')}) tip {node} wires={wt} near={[(n[0],n[1],n[2]) for n in near]}")

print()
print("=== NET3 / OUT labels: find label pins ===")
for des, info in comps.items():
    if info.get("name") in ("NET3", "OUT", "NET2", "+11.1V", "+5v", "+3.3V", "GND"):
        sym = info["symbol"]
        for pid, pin in sym_pins[sym].items():
            node = pin_tip_abs(pin, info["pos"]["x"], info["pos"]["y"], info["pos"]["rot"])
            print(f"label {info.get('name')} @ {info['pos']} pin tip {node}")

print()
print("=== U2 pin4 ON/OFF & U1 EN pin ===")
for des, pid_sel in [("U2", "4"), ("U1", "2")]:
    info = comps[des]
    for pid, pin in sym_pins[info["symbol"]].items():
        if str(pin.get("number")) != pid_sel:
            continue
        node = pin_tip_abs(pin, info["pos"]["x"], info["pos"]["y"], info["pos"]["rot"])
        near = pins_on_point(node[0], node[1], 3.0)
        wt = wires_touching(node[0], node[1])
        print(f"{des} pin {pid_sel} ({pin.get('name')}) tip {node} wires={wt} near={[(n[0],n[1],n[2]) for n in near]}")
