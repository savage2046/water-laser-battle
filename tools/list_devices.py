"""List all DEVICE documents and their objects."""
import sys
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type

entries = load_epru(r"PCB\triger-sensor-current-test_2026-08-20.epro2")
docs = docs_by_type(entries)
dev = docs.get("DEVICE", {})
print("DEVICE doc count:", len(dev))
for u, objs in dev.items():
    print("uuid:", repr(u), "objs:", len(objs))
    for c, pls in objs:
        print("   CMD type:", c.get("type"), "ticket:", c.get("ticket"), "pls:", len(pls))
        for p in pls:
            print("      PL repr:", repr(p)[:300])
