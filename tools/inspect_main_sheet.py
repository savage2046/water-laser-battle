# -*- coding: utf-8 -*-
"""Inspect U1/U2 devices and SCH attributes in gun-board main sheet."""
import json, sys, collections
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type, split_payload, deep_meta

PATH = r"PCB\gun-board_backup\gun-board_2026-08-22-16-15.epro2"
entries = load_epru(PATH)
docs = docs_by_type(entries)

print("=== DEVICE meta for U1/U2 candidates ===")
for uuid in ("509e8bb051cd0fcc", "85aed193d5487f20", "ac7e0f891c16d706"):
    objs = docs.get("DEVICE", {}).get(uuid, [])
    m = deep_meta(objs)
    print(f"\n--- DEVICE {uuid} ---")
    print(json.dumps(m, ensure_ascii=False, indent=1)[:1500])

SHEET = "4ea4588f82a06505"
objs = docs["SCH_PAGE"][SHEET]
print("\n\n=== RAW objects on main sheet ===")
for c, pls in objs:
    t = c.get("type")
    if t in ("COMPONENT", "ATTR", "WIRE", "LINE"):
        print(f"\n--- {t} id={c.get('id')} ticket={c.get('ticket')} ---")
        for p in pls:
            print("   ", json.dumps(p, ensure_ascii=False)[:400])
