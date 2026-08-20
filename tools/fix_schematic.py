"""Fix the triger-sensor schematic (.epru) inconsistencies.

Changes:
1. CN1 (XT30 male, gun-loop connector) wiring:
   - CN1.1 (+)  -> NET10 (R1 low side / INA226 VIN- VBUS)
   - CN1.2 (-)  -> GND (via CN2.2)
   - CN1.3/4 (G)-> GND
2. CN2.3 / CN2.4 (G) -> GND
3. U1 (ESP32-S3) power pins tied together: 5V pins 22/23 -> +5V,
   3.3V pins 17/18 -> +3.3V
4. Designators R2 (SDA pullup) / R3 (SCL pullup) for the two 2.2k resistors

Conflict checker: any touch (crossing) with existing geometry is allowed only
when both sides belong to the SAME net, or the point is an explicitly intended
junction (pin tips of the components being wired).
"""
import json
import sys
import zipfile
import uuid
import os
import collections

sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type
from netlist import build_symbol_pins, pin_tip_abs

SRC = r"PCB\triger-sensor-current-test_backup\triger-sensor-current-test_2026-08-19-18-40.epro2"
OUT_EPRU = r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test.epru"
OUT_EPRO2 = r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test_2026-08-19-18-40.fixed.epro2"

# ---------------------------------------------------------------- parse ----
z = zipfile.ZipFile(SRC)
names = z.namelist()
epru_name = [n for n in names if n.endswith(".epru")][0]
raw = z.read(epru_name).decode("utf-8", errors="replace")
z.close()
lines = raw.split("\n")

entries = load_epru(SRC)
docs = docs_by_type(entries)
sym_pins = build_symbol_pins(docs)
objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]

comps = {}
attrs = collections.defaultdict(dict)
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
    info["name"] = attrs[cid].get("Name")
    info["gnn"] = attrs[cid].get("Global Net Name")
    info["device"] = attrs[cid].get("Device")

def comp_by_des(des):
    for cid, info in comps.items():
        if info.get("designator") == des:
            return cid, info
    raise KeyError(des)

def pin_tips(cid, info):
    return {pid: pin_tip_abs(p, info["x"], info["y"], info["rot"]) for pid, p in sym_pins[info["symbol"]].items()}

def tips_by_number(tips, sym_uuid):
    out = {}
    for pid, t in tips.items():
        num = sym_pins[sym_uuid][pid].get("number")
        if num is not None:
            out[str(num)] = t
    return out

cn1_cid, cn1 = comp_by_des("CN1")
cn2_cid, cn2 = comp_by_des("CN2")
u1_cid, u1 = comp_by_des("U1")
cn1_by = tips_by_number(pin_tips(cn1_cid, cn1), cn1["symbol"])
cn2_by = tips_by_number(pin_tips(cn2_cid, cn2), cn2["symbol"])
u1_by = tips_by_number(pin_tips(u1_cid, u1), u1["symbol"])

# ------------------------------------------------------- net per wire ----
# union-find over wire nodes; bridge wires via pins; name nets via labels
node_wires = collections.defaultdict(set)
wire_nodes = collections.defaultdict(set)
for wid, segs in wires.items():
    for (x1, y1, x2, y2) in segs:
        a = (round(x1, 3), round(y1, 3))
        b = (round(x2, 3), round(y2, 3))
        node_wires[a].add(wid)
        node_wires[b].add(wid)
        wire_nodes[wid].add(a)
        wire_nodes[wid].add(b)

parent = {}
def find(a):
    parent.setdefault(a, a)
    while parent[a] != a:
        parent[a] = parent[parent[a]]
        a = parent[a]
    return a
def union(a, b):
    ra, rb = find(a), find(b)
    if ra != rb:
        parent[ra] = rb

for wid, nodes in wire_nodes.items():
    nodes = list(nodes)
    for i in range(1, len(nodes)):
        union(nodes[0], nodes[i])

pin_of_node = collections.defaultdict(list)
for cid, info in comps.items():
    sym = info.get("symbol")
    if sym not in sym_pins:
        continue
    for pid, pin in sym_pins[sym].items():
        node = pin_tip_abs(pin, info["x"], info["y"], info["rot"])
        pin_of_node[node].append((cid, pid))

# bridge wires that share a pin node
for node, pins in pin_of_node.items():
    wids = list(node_wires.get(node, ()))
    for i in range(1, len(wids)):
        union(node, node)  # no-op
    # a pin node joins all wires touching it: union their nodes
    for w in wids:
        for n2 in wire_nodes[w]:
            union(node, n2)
# label symbols (no designator + gnn) provide net names
label_roots = {}
for cid, info in comps.items():
    nm = info.get("gnn")
    if nm and not info.get("designator"):
        sym = info.get("symbol")
        if sym in sym_pins:
            for pid in sym_pins[sym]:
                node = pin_tip_abs(sym_pins[sym][pid], info["x"], info["y"], info["rot"])
                label_roots[find(node)] = nm

def net_of_wire(wid):
    if not wire_nodes[wid]:
        return wid
    root = find(next(iter(wire_nodes[wid])))
    return label_roots.get(root, root)

def net_of_point(px, py):
    n = (round(px, 3), round(py, 3))
    r = find(n)
    return label_roots.get(r, r)

# net of every pin tip
pin_net = {}
for node, pins in pin_of_node.items():
    r = find(node)
    nm = label_roots.get(r, r)
    for (cid, pid) in pins:
        pin_net[(cid, pid)] = nm

# ------------------------------------------------------------- routes ----
routes = [
    # CN1.1 (+) -> NET10 wire 79a1ccb0 endpoint (915,-425)
    ("CN1.1(+) -> NET10", cn1_by["1"], "NET10",
     [(955, -415, 955, -380), (955, -380, 930, -380), (930, -380, 930, -425), (930, -425, 915, -425)]),
    # CN1.2 (-) -> GND at CN2.2 tip (900,-530); split at (920,-490) so CN1.4
    # has a junction node to hook onto
    ("CN1.2(-) -> GND", cn1_by["2"], "GND",
     [(955, -425, 955, -490), (955, -490, 920, -490), (920, -490, 900, -490), (900, -490, 900, -530)]),
    # CN1.3 (G) -> GND wire 5e47c0a5 (915,-415)
    ("CN1.3(G) -> GND", cn1_by["3"], "GND",
     [(920, -395, 915, -395), (915, -395, 915, -415)]),
    # CN1.4 (G) -> CN1.2 wire at (920,-490)
    ("CN1.4(G) -> GND", cn1_by["4"], "GND",
     [(920, -445, 920, -490)]),
    # CN2.3 (G) -> GND label tip (890,-515)
    ("CN2.3(G) -> GND", cn2_by["3"], "GND",
     [(930, -510, 905, -510), (905, -510, 905, -515), (905, -515, 890, -515)]),
    # CN2.4 (G) -> GND at CN2.2 tip (900,-530)
    ("CN2.4(G) -> GND", cn2_by["4"], "GND",
     [(930, -560, 910, -560), (910, -560, 910, -530), (910, -530, 900, -530)]),
    # U1 5V pins 22/23 -> pin24 tip (378,-509) already on +5V;
    # segment split at pin23 (378,-499) to create a junction node
    ("U1 5V -> +5V", u1_by["22"], "+5V",
     [(378, -489, 378, -499), (378, -499, 378, -509)]),
    # U1 3.3V pins 17/18 -> pin19 tip (260,-489) already on +3.3V;
    # segment split at pin18 (260,-499) to create a junction node
    ("U1 3.3V -> +3.3V", u1_by["17"], "+3.3V",
     [(260, -509, 260, -499), (260, -499, 260, -489)]),
    # NOTE: U1 GND pins (7/20/21/32) are floating in the original layout.
    # They are boxed in by component pins and the I2C wire verticals, so any
    # route would short other nets. Fixing requires layout changes in the EDA
    # GUI (see report); intentionally NOT routed here.
]

dest_net_of_wire = {"NET10": net_of_wire("79a1ccb0a071a469")}
print("NET10 wire net:", dest_net_of_wire["NET10"])
dest_net_of_wire["GND"] = "GND"
dest_net_of_wire["+5V"] = "+5V"
dest_net_of_wire["+3.3V"] = "+3.3V"

# pins intentionally joined (start pins of routes + pass-through pins)
intended_pins = {
    (955.0, -415.0), (955.0, -425.0), (920.0, -395.0), (920.0, -445.0),
    (930.0, -510.0), (930.0, -560.0), (378.0, -489.0), (260.0, -509.0),
    (378.0, -499.0), (260.0, -499.0),  # pass-through junctions (pins 23, 18)
    (900.0, -530.0), (890.0, -515.0), (378.0, -509.0), (260.0, -489.0),
}

existing_segs = []
for wid, segs in wires.items():
    for s in segs:
        existing_segs.append((wid, s[0], s[1], s[2], s[3]))

all_pin_tips = set()
for cid, info in comps.items():
    sym = info.get("symbol")
    if sym in sym_pins:
        for pid, pin in sym_pins[sym].items():
            all_pin_tips.add(pin_tip_abs(pin, info["x"], info["y"], info["rot"]))

# ------------------------------------------------------------------ check --
def seg_intersection_point(a, b):
    """Return one intersection/touch point of two axis-aligned segments, or None."""
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    if ax1 > ax2: ax1, ax2 = ax2, ax1
    if ay1 > ay2: ay1, ay2 = ay2, ay1
    if bx1 > bx2: bx1, bx2 = bx2, bx1
    if by1 > by2: by1, by2 = by2, by1
    if ay1 == ay2 and by1 == by2 and ay1 == by1:
        lo, hi = max(ax1, bx1), min(ax2, bx2)
        return ((lo + hi) / 2, ay1) if lo <= hi + 1e-9 else None
    if ax1 == ax2 and bx1 == bx2 and ax1 == bx1:
        lo, hi = max(ay1, by1), min(ay2, by2)
        return (ax1, (lo + hi) / 2) if lo <= hi + 1e-9 else None
    if ay1 == ay2 and bx1 == bx2:
        if ax1 - 1e-9 <= bx1 <= ax2 + 1e-9 and by1 - 1e-9 <= ay1 <= by2 + 1e-9:
            return (bx1, ay1)
        return None
    if ax1 == ax2 and by1 == by2:
        if bx1 - 1e-9 <= ax1 <= bx2 + 1e-9 and ay1 - 1e-9 <= by1 <= ay2 + 1e-9:
            return (ax1, by1)
        return None
    return None

def is_touch(ip, endpoints):
    return any(abs(ip[0] - p[0]) < 1e-6 and abs(ip[1] - p[1]) < 1e-6 for p in endpoints)

def point_on_seg(px, py, s):
    x1, y1, x2, y2 = s
    if x1 > x2: x1, x2 = x2, x1
    if y1 > y2: y1, y2 = y2, y1
    if x1 == x2:
        return abs(px - x1) < 1e-9 and y1 - 1e-9 <= py <= y2 + 1e-9
    if y1 == y2:
        return abs(py - y1) < 1e-9 and x1 - 1e-9 <= px <= x2 + 1e-9
    return False

def is_intended(ip):
    return any(abs(ip[0] - q[0]) < 1e-6 and abs(ip[1] - q[1]) < 1e-6 for q in intended_pins)

problems = []
for name, start, dest, segs in routes:
    dest_net = dest_net_of_wire[dest]
    print(f"\n=== route {name} (dest net={dest_net})")
    for s in segs:
        # against existing wires
        for ex in existing_segs:
            wid = ex[0]
            ip = seg_intersection_point(s, ex[1:])
            if ip is None:
                continue
            wnet = net_of_wire(wid)
            same_net = (wnet == dest_net)
            touch = is_touch(ip, [(s[0], s[1]), (s[2], s[3]), (ex[1], ex[2]), (ex[3], ex[4])])
            if same_net or (touch and is_intended(ip)):
                print(f"   ok: wire {wid[:8]} net={wnet} at {ip}")
            else:
                msg = f"   CONFLICT: {name} seg {s} touches wire {wid[:8]} net={wnet} at {ip}"
                print(msg)
                problems.append(msg)
        # against existing pin tips
        for tip in all_pin_tips:
            if not point_on_seg(tip[0], tip[1], s):
                continue
            if tip in intended_pins:
                continue
            pn = net_of_point(tip[0], tip[1])
            if pn == dest_net:
                print(f"   ok: pin {tip} (net {pn}) junction")
                continue
            msg = f"   CONFLICT: {name} seg {s} passes pin tip {tip} (net {pn})"
            print(msg)
            problems.append(msg)
        # against other new routes
        for other_name, other_start, other_dest, other_segs in routes:
            if other_name == name:
                continue
            for os_ in other_segs:
                ip = seg_intersection_point(s, os_)
                if ip is None:
                    continue
                other_net = dest_net_of_wire[other_dest]
                same_net = (other_net == dest_net)
                touch = is_touch(ip, [(s[0], s[1]), (s[2], s[3]), (os_[0], os_[1]), (os_[2], os_[3])])
                if same_net or (touch and is_intended(ip)):
                    print(f"   ok: junction {other_name} at {ip} (both {dest_net})")
                else:
                    msg = f"   CONFLICT: {name} seg {s} crosses {other_name} seg {os_} at {ip} ({dest_net} vs {other_net})"
                    print(msg)
                    problems.append(msg)

if problems:
    print("\n!!! ROUTE CONFLICTS FOUND - aborting fix !!!")
    sys.exit(1)
print("\nAll routes clear.")

# -------------------------------------------------------------- edit ----
ticket = 5184
new_lines = []
zi = 200
for name, start, dest, segs in routes:
    wid = uuid.uuid4().hex
    ticket += 1
    new_lines.append('{"type":"WIRE","ticket":%d,"id":"%s"}||{"zIndex":%d}|' % (ticket, wid, zi))
    zi += 1
    for (x1, y1, x2, y2) in segs:
        ticket += 1
        new_lines.append(
            '{"type":"LINE","ticket":%d,"id":"%s"}||{"fillColor":null,"fillStyle":null,'
            '"strokeColor":null,"strokeStyle":null,"strokeWidth":null,'
            '"startX":%g,"startY":%g,"endX":%g,"endY":%g,"lineGroup":"%s"}|'
            % (ticket, uuid.uuid4().hex, x1, y1, x2, y2, wid))
    ticket += 1
    new_lines.append(
        '{"type":"ATTR","ticket":%d,"id":"%s"}||{"x":null,"y":null,"rotation":null,'
        '"color":null,"fontFamily":null,"fontSize":null,"fontWeight":null,"italic":null,'
        '"underline":null,"align":null,"value":"[]","keyVisible":null,"valueVisible":null,'
        '"key":"Relevance","fillColor":null,"parentId":"%s"}|'
        % (ticket, uuid.uuid4().hex, wid))
    ticket += 1
    new_lines.append(
        '{"type":"ATTR","ticket":%d,"id":"%s"}||{"x":%g,"y":%g,"rotation":0,"color":null,'
        '"fontFamily":null,"fontSize":null,"fontWeight":null,"italic":null,"underline":null,'
        '"align":null,"value":"","keyVisible":false,"valueVisible":true,'
        '"key":"NET","fillColor":null,"parentId":"%s"}|'
        % (ticket, uuid.uuid4().hex, segs[0][0], segs[0][1], wid))

print(f"\nnew lines: {len(new_lines)}, tickets 5185..{ticket}")

start_i = end_i = None
for i, ln in enumerate(lines):
    if '"docType":"SCH_PAGE"' in ln:
        start_i = i
    if start_i is not None and i > start_i and '"docType"' in ln and '"SCH_PAGE"' not in ln:
        end_i = i
        break
assert start_i is not None and end_i is not None
print(f"SCH_PAGE lines {start_i}..{end_i-1}; inserting {len(new_lines)} lines at {end_i}")

# --- designator fix: identify the two 2.2k pullups by their pin nets ---
sda_wire_net = dest_net_of_wire["NET10"]  # placeholder; real: find SDA wire
# SDA net = net of U3 pin4 (SDA); SCL = net of U3 pin5
def net_of_comp_pin(des, num):
    cid, info = comp_by_des(des)
    tips = pin_tips(cid, info)
    for pid, tip in tips.items():
        if str(sym_pins[info["symbol"]][pid].get("number")) == str(num):
            return net_of_point(tip[0], tip[1])
    return None

sda_net = net_of_comp_pin("U3", "4")
scl_net = net_of_comp_pin("U3", "5")
print("U3 SDA net:", sda_net, " SCL net:", scl_net)

r_sda = r_scl = None
for cid, info in comps.items():
    if info.get("designator") or info.get("symbol") != "61e0c260dec88968":
        continue
    tips = pin_tips(cid, info)
    pin_nets = set()
    for pid, tip in tips.items():
        pin_nets.add(net_of_point(tip[0], tip[1]))
    if sda_net in pin_nets and scl_net not in pin_nets:
        r_sda = cid
    elif scl_net in pin_nets and sda_net not in pin_nets:
        r_scl = cid
print("SDA pullup:", r_sda, " SCL pullup:", r_scl)
assert r_sda and r_scl and r_sda != r_scl

def set_designator(comp_id, new_des):
    for i in range(start_i, end_i):
        ln = lines[i]
        if '"key":"Designator"' not in ln or '"parentId":"%s"' % comp_id not in ln:
            continue
        assert '"value":null' in ln, f"line {i} has no null value"
        lines[i] = ln.replace('"value":null', '"value":"%s"' % new_des, 1)
        print(f"designator {new_des} set on line {i}")
        return
    raise RuntimeError(f"Designator attr not found for {comp_id}")

set_designator(r_sda, "R2")
set_designator(r_scl, "R3")

# --- placeholder max bumps ---
def bump_placeholder(ph_id, delta):
    for i in range(start_i, end_i):
        ln = lines[i]
        if '"id":"%s"' % ph_id in ln and '"max":' in ln:
            import re
            m = re.search(r'"max":(\d+)', ln)
            if m:
                newmax = int(m.group(1)) + delta
                lines[i] = ln.replace(m.group(0), '"max":%d' % newmax, 1)
                print(f"placeholder {ph_id} max -> {newmax}")
            return
    print(f"WARN: placeholder {ph_id} not found")

bump_placeholder("placeholder13", 14)
bump_placeholder("placeholder8", 30)
bump_placeholder("placeholder21", 30)

lines[end_i:end_i] = new_lines

# ------------------------------------------------------------- write ----
os.makedirs(os.path.dirname(OUT_EPRU), exist_ok=True)
new_epru = "\n".join(lines)
with open(OUT_EPRU, "w", encoding="utf-8") as f:
    f.write(new_epru)
print(f"\nwrote {OUT_EPRU} ({len(lines)} lines)")

z = zipfile.ZipFile(SRC)
with zipfile.ZipFile(OUT_EPRO2, "w", zipfile.ZIP_DEFLATED) as zout:
    for n in names:
        if n == epru_name:
            zout.writestr(n, new_epru)
        else:
            zout.writestr(n, z.read(n))
z.close()
print(f"wrote {OUT_EPRO2}")
