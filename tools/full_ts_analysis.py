"""Full analysis of triger-sensor final design: BOM + netlist + module pinout + power chain."""
import os
import sys
import json
import collections
import math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_full_report import load_epru, docs_by_type
from netlist import build_symbol_pins, pin_tip_abs, rotate, build_netlist

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

devs = {}
for uuid, dlist in docs.get("DEVICE", {}).items():
    for c, pls in dlist:
        if c.get("type") == "META" and pls:
            m = pls[0]
            if isinstance(m, dict):
                devs[uuid] = m

sym_pins = build_symbol_pins(docs)

print("=" * 90)
print("COMPONENT DETAIL (designator / device / value / lcsc / fp)")
print("=" * 90)
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
    dev = devs.get(a.get("Device"), {}) or {}
    dev_attrs = dev.get("attributes") or {}
    comps[des] = {
        "id": cid, "pos": pos,
        "device": dev.get("title"),
        "value": a.get("Value") or dev_attrs.get("Value"),
        "lcsc": a.get("LCSC Part Number") or dev_attrs.get("Supplier Part"),
        "fp": a.get("Footprint") or dev_attrs.get("Supplier Footprint"),
        "mpn": dev_attrs.get("Manufacturer Part"),
        "mfr": dev_attrs.get("Manufacturer"),
        "symbol": a.get("Symbol"),
    }
    print(f"  {des:6} | {str(comps[des]['device']):38} | {str(comps[des]['value']):14} | C{comps[des]['lcsc']} | {comps[des]['fp']}")

print()
print("=" * 90)
print("CONNECTOR PIN TIPS (CN1/CN2) + wires nearby")
print("=" * 90)
# all wires as segments
wires = collections.defaultdict(list)
for c, pls in objs:
    if c.get("type") != "LINE":
        continue
    for pp in pls:
        if isinstance(pp, dict) and pp.get("lineGroup") and pp.get("startX") is not None:
            wires[pp["lineGroup"]].append(
                (pp["startX"], pp["startY"], pp["endX"], pp["endY"]))

for des in ["CN1", "CN2"]:
    if des not in comps:
        continue
    info = comps[des]
    sym = info["symbol"]
    print(f"--- {des} device={info['device']} pos={info['pos']}")
    if sym not in sym_pins:
        print("   (no symbol pins)")
        continue
    for pid, pin in sym_pins[sym].items():
        node = pin_tip_abs(pin, info["pos"]["x"], info["pos"]["y"], info["pos"]["rot"])
        print(f"   pin {pid}: num={pin.get('number')} name={pin.get('name')} tip={node}")

print()
print("=" * 90)
print("U1 ESP32-C3-WROOM-02-N4 PIN MAP (pin num -> name -> net)")
print("=" * 90)
# rebuild netlist properly
comps2, nets, labels = build_netlist(entries)
# map designator -> pin name -> net
des2cid = {}
for cid, info in comps2.items():
    des2cid.setdefault(info.get("designator"), cid)
sym = comps["U1"]["symbol"]
info = comps2[des2cid["U1"]]
for pid, pin in sym_pins[sym].items():
    node = pin_tip_abs(pin, info["x"], info["y"], info["rot"])
    net = None
    for nm, conns in nets.items():
        for d, num, pname in conns:
            if d == "U1" and str(num) == str(pin.get("number")):
                net = nm
    print(f"   pin {str(pin.get('number')):3} {str(pin.get('name')):8} -> {net}")
