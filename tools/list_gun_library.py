# -*- coding: utf-8 -*-
"""List all DEVICE/FOOTPRINT/SYMBOL docs in gun-board epro2 (library reuse assessment)."""
import sys, json
sys.path.insert(0, "tools")
from extract_full_report import load_epru, docs_by_type, deep_meta

entries = load_epru(r"PCB\gun-board_backup\gun-board_2026-08-22-16-15.epro2")
docs = docs_by_type(entries)

print("=== DEVICE library (title / LCSC Part / Footprint) ===")
for uuid, objs in docs.get("DEVICE", {}).items():
    m = deep_meta(objs) or {}
    attrs = m.get("attributes", {}) or {}
    print(f"{uuid} | {str(m.get('title'))[:44]:<44} | {str(attrs.get('Supplier Part'))[:12]:<12} | {str(attrs.get('Manufacturer Part'))[:28]:<28} | fp={str(attrs.get('Footprint'))[:16]}")

print("\n=== SYMBOL count:", len(docs.get("SYMBOL", {})), " FOOTPRINT count:", len(docs.get("FOOTPRINT", {})))
print("\n=== FOOTPRINT docs ===")
for uuid, objs in docs.get("FOOTPRINT", {}).items():
    m = deep_meta(objs) or {}
    print(f"{uuid} | {str(m.get('title'))[:60]}")
