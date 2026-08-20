"""Explore coordinates needed for the schematic fix."""
import sys
import collections
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type
from netlist import build_symbol_pins, pin_tip_abs

SRC = r"PCB\triger-sensor-current-test_backup\triger-sensor-current-test_2026-08-19-18-40.epro2"
entries = load_epru(SRC)
docs = docs_by_type(entries)
sym_pins = build_symbol_pins(docs)
objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]

comps = {}
attrs = collections.defaultdict(dict)
wires = collections.defaultdict(list)
wire_attr = {}
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
    elif t == "WIRE":
        for p in pls:
            if isinstance(p, dict) and p.get("NET"):
                wire_attr[c.get("id")] = p["NET"]

for cid, info in comps.items():
    info["designator"] = attrs[cid].get("Designator")
    info["symbol"] = attrs[cid].get("Symbol")
    info["name"] = attrs[cid].get("Name")
    info["gnn"] = attrs[cid].get("Global Net Name")
    info["device"] = attrs[cid].get("Device")

# nets of pins via geometry
node_wires = collections.defaultdict(set)
wire_nodes = collections.defaultdict(set)
for wid, segs in wires.items():
    for (x1, y1, x2, y2) in segs:
        a = (round(x1, 3), round(y1, 3)); b = (round(x2, 3), round(y2, 3))
        node_wires[a].add(wid); node_wires[b].add(wid)
        wire_nodes[wid].add(a); wire_nodes[wid].add(b)

print("=== COMPONENTS (designator, pos, rot, symbol) ===")
for cid, info in sorted(comps.items(), key=lambda kv: str(kv[1].get("designator"))):
    print(f'{str(info["designator"]):<6} {str(info["name"] or ""):<8} pos({info["x"]},{info["y"]}) rot{info["rot"]} sym={info["symbol"][:8]} dev={str(info["device"])[:10]}')

print()
print("=== CN1 pin endpoints (XT30 male) ===")
cn1 = next((cid, i) for cid, i in comps.items() if i.get("designator") == "CN1")
for pid, pin in sorted(sym_pins[cn1[1]["symbol"]].items(), key=lambda kv: str(kv[1].get("number"))):
    tip = pin_tip_abs(pin, cn1[1]["x"], cn1[1]["y"], cn1[1]["rot"])
    print(f'  pin {pin["number"]} ({pin["name"]}) -> {tip}')

print()
print("=== CN2 pin endpoints (XT30 female) ===")
cn2 = next((cid, i) for cid, i in comps.items() if i.get("designator") == "CN2")
for pid, pin in sorted(sym_pins[cn2[1]["symbol"]].items(), key=lambda kv: str(kv[1].get("number"))):
    tip = pin_tip_abs(pin, cn2[1]["x"], cn2[1]["y"], cn2[1]["rot"])
    print(f'  pin {pin["number"]} ({pin["name"]}) -> {tip}')

print()
print("=== U1 pin endpoints (ESP32-S3 dev board) ===")
u1 = next((cid, i) for cid, i in comps.items() if i.get("designator") == "U1")
for pid, pin in sorted(sym_pins[u1[1]["symbol"]].items(), key=lambda kv: str(kv[1].get("number"))):
    tip = pin_tip_abs(pin, u1[1]["x"], u1[1]["y"], u1[1]["rot"])
    print(f'  pin {pin["number"]:>2} ({pin["name"]}) -> {tip}')

print()
print("=== wires with endpoints (id: segments) ===")
for wid in sorted(wires, key=lambda w: len(wires[w]), reverse=True):
    print(f"  {wid} net={wire_attr.get(wid, '')!r}")
    for s in wires[wid]:
        print(f"      ({s[0]},{s[1]}) -> ({s[2]},{s[3]})")

print()
print("=== net labels (component name/gnn + pin endpoints) ===")
for cid, info in comps.items():
    if info.get("gnn"):
        sym = info["symbol"]
        tips = [pin_tip_abs(p, info["x"], info["y"], info["rot"]) for p in sym_pins[sym].values()]
        print(f'  {info["gnn"]:<8} label sym at ({info["x"]},{info["y"]}) pins: {tips}')
