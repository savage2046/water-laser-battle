"""Build a geometric netlist from a JLCEDA Pro .epru schematic (SCH_PAGE)."""
import json
import sys
import zipfile
import collections
import math

from extract_full_report import load_epru, docs_by_type, split_payload


def rotate(x, y, deg):
    r = math.radians(deg)
    return (x * math.cos(r) - y * math.sin(r), x * math.sin(r) + y * math.cos(r))


def build_symbol_pins(docs):
    """{sym_uuid: {pin_id: {name, number, x, y, rot, length}}}"""
    out = {}
    for uuid, objs in docs.get("SYMBOL", {}).items():
        pins = {}
        attrs = collections.defaultdict(dict)
        for c, pls in objs:
            t = c.get("type")
            if t == "PIN":
                p = pls[0] if pls and isinstance(pls[0], dict) else {}
                pins[c.get("id")] = {
                    "x": p.get("x", 0), "y": p.get("y", 0),
                    "rot": p.get("rotation", 0), "length": p.get("length", 0),
                }
            elif t == "ATTR":
                for p in pls:
                    if isinstance(p, dict) and p.get("parentId"):
                        attrs[p["parentId"]][p.get("key")] = p.get("value")
        for pid, pin in pins.items():
            pin["name"] = attrs[pid].get("Pin Name")
            pin["number"] = attrs[pid].get("Pin Number")
        out[uuid] = pins
    return out


def pin_tip_abs(pin, cx, cy, crot):
    """Absolute position of the pin's connectable end.

    In JLCEDA Pro symbols the PIN (x,y) IS the connectable endpoint
    (the drawn 'length' extends from it toward the body), so no offset
    is added; only the component rotation is applied.
    """
    rx, ry = rotate(pin["x"], pin["y"], crot)
    return (round(cx + rx, 3), round(cy + ry, 3))


def build_netlist(entries, sch_uuid="ca1c10d680cb80bb"):
    docs = docs_by_type(entries)
    sym_pins = build_symbol_pins(docs)
    objs = docs["SCH_PAGE"][sch_uuid]

    comps = {}   # comp_id -> info
    attrs = collections.defaultdict(dict)
    wires = collections.defaultdict(list)  # wire_id -> [(x1,y1,x2,y2)]
    net_attr = {}  # wire_id -> net name (from NET ATTR)

    for c, pls in objs:
        t = c.get("type")
        if t == "COMPONENT":
            p = pls[0] if pls and isinstance(pls[0], dict) else {}
            comps[c.get("id")] = {
                "partId": p.get("partId"),
                "x": p.get("x", 0), "y": p.get("y", 0),
                "rot": p.get("rotation", 0),
            }
        elif t == "ATTR":
            for p in pls:
                if isinstance(p, dict) and p.get("parentId"):
                    attrs[p["parentId"]][p.get("key")] = p.get("value")
        elif t == "LINE":
            for p in pls:
                if isinstance(p, dict) and p.get("lineGroup") and p.get("startX") is not None:
                    wires[p["lineGroup"]].append(
                        (p["startX"], p["startY"], p["endX"], p["endY"]))
        elif t == "WIRE":
            for p in pls:
                if isinstance(p, dict) and p.get("NET"):
                    net_attr[c.get("id")] = p["NET"]

    # component details
    for cid, info in comps.items():
        info["designator"] = attrs[cid].get("Designator")
        info["symbol"] = attrs[cid].get("Symbol")
        info["device"] = attrs[cid].get("Device")
        info["name"] = attrs[cid].get("Name")
        info["gnn"] = attrs[cid].get("Global Net Name")
        info["footprint"] = attrs[cid].get("Footprint")

    # ---- net labels: components that carry a Name/Global Net Name ----
    labels = {}  # comp_id -> net name
    for cid, info in comps.items():
        nm = info.get("gnn") or info.get("name")
        if nm and (info.get("device") or "").startswith(("Power", "Ground")) or (nm and not info.get("designator")):
            # net label symbols have no designator
            if nm and not info.get("designator"):
                labels[cid] = nm

    # ---- wire endpoint nodes ----
    # node -> wire ids
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

    # ---- pins: comp.pin -> tip node; node -> pins ----
    pin_of_node = collections.defaultdict(list)
    comp_pin_tip = {}  # (comp_id, pin_id) -> node
    for cid, info in comps.items():
        sym = info.get("symbol")
        if not sym or sym not in sym_pins:
            continue
        for pid, pin in sym_pins[sym].items():
            node = pin_tip_abs(pin, info["x"], info["y"], info["rot"])
            comp_pin_tip[(cid, pid)] = node
            pin_of_node[node].append((cid, pid))

    # ---- labels attach: label comp pin tip node ----
    label_nodes = {}
    for cid, nm in labels.items():
        sym = comps[cid].get("symbol")
        if sym in sym_pins:
            for pid in sym_pins[sym]:
                label_nodes[comp_pin_tip[(cid, pid)]] = nm

    # ---- union-find over wires (via shared nodes) ----
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

    # union nodes within the same wire
    for wid, nodes in wire_nodes.items():
        nodes = list(nodes)
        for i in range(1, len(nodes)):
            union(nodes[0], nodes[i])
    # a pin joins its node to the wire net; union label node with wire
    for node, pins in pin_of_node.items():
        for wid in node_wires.get(node, ()):
            pass
    # union all nodes that share a wire, plus nodes that touch via pins:
    # a wire node connects to another wire only through pins, so merge:
    for node, pins in pin_of_node.items():
        for wid in node_wires.get(node, ()):
            union(node, node)  # no-op safeguard
    # merge wire nodes that are bridged by a pin (pin touching two wires):
    for node, pins in pin_of_node.items():
        for wid in node_wires.get(node, ()):
            for n2 in wire_nodes[wid]:
                union(node, n2)
    # also connect two nodes directly bridged by a pin (no wire)
    for node, pins in pin_of_node.items():
        for (cid, pid) in pins:
            pass
    # label node joins its wire
    for node, nm in label_nodes.items():
        for wid in node_wires.get(node, ()):
            for n2 in wire_nodes[wid]:
                union(node, n2)

    # ---- assign net names: BFS from label nodes ----
    root_net = {}
    for node, nm in label_nodes.items():
        root_net[find(node)] = nm
    # unnamed nets get generated names
    idx = 1
    nets = collections.defaultdict(set)  # net name -> set of (designator, pin num/name)
    comp_by_id = comps

    def net_of_node(node):
        r = find(node)
        if r in root_net:
            return root_net[r]
        return None

    seen = set()
    for (cid, pid), node in comp_pin_tip.items():
        r = find(node)
        nm = root_net.get(r)
        if nm is None:
            # assign anonymous net id
            if r not in root_net:
                root_net[r] = f"NET{idx}"
                idx += 1
            nm = root_net[r]
        info = comp_by_id[cid]
        des = info.get("designator") or info.get("name") or info.get("partId") or cid
        pin = sym_pins[info.get("symbol")][pid]
        nets[nm].add((des, pin.get("number"), pin.get("name")))

    return comps, nets, labels


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else r"PCB\triger-sensor-current-test_backup\triger-sensor-current-test_2026-08-19-18-40.epro2"
    entries = load_epru(path)
    comps, nets, labels = build_netlist(entries)

    print("=== NETLIST ===")
    for nm in sorted(nets, key=lambda s: (s.startswith("NET"), s)):
        conns = sorted(nets[nm])
        print(f"\n[{nm}]  ({len(conns)} pins)")
        for des, num, name in conns:
            print(f"    {des:<12} pin {str(num):<4} ({name})")
