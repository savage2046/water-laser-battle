"""Debug symbol pin extraction for new symbols."""
import sys
import json
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type

entries = load_epru(r"PCB\triger-sensor-current-test_2026-08-20.epro2")
docs = docs_by_type(entries)

syms = docs.get("SYMBOL", {})
print("SYMBOL count:", len(syms))
for uuid in ["edf6353dd3b6", "c8eb57bd4ba0", "ca82ee4c7119", "bc270ee86fac"]:
    full = [u for u in syms if u.startswith(uuid)]
    print(f"uuid prefix {uuid}: {full}")
    if full:
        for c, pls in syms[full[0]]:
            if c.get("type") in ("PIN", "META", "PART"):
                print("  ", c.get("type"), json.dumps(pls[0] if pls else None, ensure_ascii=False)[:400])
