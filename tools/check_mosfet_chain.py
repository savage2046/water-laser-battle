"""Check U4/Q1/R4 wiring: pin names, tips, and nearby wires."""
import sys
import collections
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type
from netlist import build_symbol_pins, pin_tip_abs

entries = load_epru(r"PCB\triger-sensor-current-test_2026-08-20.epro2")
docs = docs_by_type(entries)
sym_pins = build_symbol_pins(docs)

objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]
attrs = collections.defaultdict(dict)
comps = {}
wires = collections.defaultdict(list)
for c, pls in objs:
    t = c.get("type")
    if t == "COMPONENT":
        p = pls[0] if pls and isinstance(pls[0], dict) else {}
        comps[c.get("id")] = {"partId": p.get("partId"), "x": p.get("x"), "y": p.get("y"), "rot": p.get("rotation")}
    elif t == "ATTR":
        for p in pls:
            if isinstance(p, dict) and p.get("parentId"):
                attrs[p["parentId"]][p.get("key")] = p.get("value")
    elif t == "LINE":
        for p in pls:
            if isinstance(p, dict) and p.get("lineGroup") and p.get("startX") is not None:
                wires[p["lineGroup"]].append((p["startX"], p["startY"], p["endX"], p["endY"]))
for cid, info in comps.items():
    info["designator"] = attrs[cid].get("Designator")
    info["symbol"] = attrs[cid].get("Symbol")

print("=== U4 / Q1 / R4 / R5 pin tips ===")
for cid, info in comps.items():
    if info.get("designator") in ("U4", "Q1", "R4", "R5"):
        sym = info.get("symbol")
        print(f"{info['designator']} pos=({info['x']},{info['y']}) rot={info['rot']} sym={sym}")
        if sym in sym_pins:
            for pid, pin in sorted(sym_pins[sym].items(), key=lambda kv: str(kv[1].get("number"))):
                tip = pin_tip_abs(pin, info["x"], info["y"], info["rot"])
                print(f"   pin {pin['number']} ({pin['name']}) tip={tip}")
        else:
            print("   (no pin table for symbol)")

print("\n=== wires with x >= 380 ===")
for wid, segs in wires.items():
    for (x1, y1, x2, y2) in segs:
        if min(x1, x2) >= 380 or min(y1, y2) <= -700:
            print(f"  {wid[:8]} ({x1},{y1})->({x2},{y2})")
