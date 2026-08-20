"""Extract components and new device metadata from updated schematic."""
import sys
import collections
import json
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type

entries = load_epru(r"PCB\triger-sensor-current-test_2026-08-20.epro2")
docs = docs_by_type(entries)

objs = docs["SCH_PAGE"]["ca1c10d680cb80bb"]
attrs = collections.defaultdict(dict)
for c, pls in objs:
    if c.get("type") != "ATTR":
        continue
    for p in pls:
        if isinstance(p, dict) and p.get("parentId") and p.get("key") is not None:
            attrs[p["parentId"]][p["key"]] = p.get("value")

print("=== SCH_PAGE COMPONENTS ===")
for c, pls in objs:
    if c.get("type") != "COMPONENT":
        continue
    cid = c.get("id")
    at = attrs.get(cid, {})
    des = at.get("Designator")
    print(f"{str(des):<8} dev={str(at.get('Device'))[:12]} sym={str(at.get('Symbol'))[:12]} "
          f"name={at.get('Name')!r} lcsc={at.get('LCSC Part Number')}")

print()
print("=== NEW DEVICE metadata ===")
for uuid in ["876106cee09f", "148a7a637967", "245468e2160e", "5da211544258"]:
    found = False
    for c, pls in docs["DEVICE"].get(uuid, []):
        if c.get("type") == "META" and pls:
            print(uuid, "->", json.dumps(pls[0], ensure_ascii=False)[:700])
            found = True
            break
    if not found:
        print(uuid, "-> (no META)")
