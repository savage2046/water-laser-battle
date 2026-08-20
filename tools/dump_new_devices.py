"""Dump DEVICE document contents for the new devices."""
import sys
import json
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type

entries = load_epru(r"PCB\triger-sensor-current-test_2026-08-20.epro2")
docs = docs_by_type(entries)

for uuid in ["876106cee09f", "148a7a637967", "245468e2160e", "5da211544258"]:
    print("=" * 70)
    print("DEVICE", uuid)
    for c, pls in docs["DEVICE"].get(uuid, []):
        print(" CMD:", json.dumps(c, ensure_ascii=False)[:200])
        for p in pls:
            if isinstance(p, str):
                print(" PL(str):", p[:200])
            else:
                print(" PL:", json.dumps(p, ensure_ascii=False)[:1500])
