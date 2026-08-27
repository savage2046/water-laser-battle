# -*- coding: utf-8 -*-
"""Inspect gun-board.eprj2: history_data storage format & main board docs."""
import sqlite3, base64, json, sys

DB = r"PCB\gun-board.eprj2"
con = sqlite3.connect(DB)
con.text_factory = lambda b: b.decode("utf-8", "replace")
cur = con.cursor()

print("=== history_data tail (latest 12) ===")
cur.execute("SELECT id, uuid, history_uuid, length(dataStr) FROM history_data ORDER BY id DESC LIMIT 12")
for r in cur.fetchall():
    print(r)

print("\n=== try base64 decode of a few rows ===")
cur.execute("SELECT id, dataStr FROM history_data ORDER BY id DESC LIMIT 5")
rows = cur.fetchall()
for rid, raw in rows:
    try:
        dec = base64.b64decode(raw)
        head = dec[:80]
        printable = sum(1 for b in head if 32 <= b < 127)
        print(f"id={rid} len={len(raw)} decoded={len(dec)} head={head!r} printable_ratio={printable/len(head):.2f}")
    except Exception as e:
        print(f"id={rid} err={e}")

print("\n=== project_history rows for main branch (first 5) ===")
cur.execute("SELECT id, uuid, key, snapshot, num, is_lock FROM project_history_352d71de19254034a294e1ad96096716 ORDER BY id DESC LIMIT 5")
for r in cur.fetchall():
    print(r)

print("\n=== all distinct history_uuid in history_data ===")
cur.execute("SELECT history_uuid, COUNT(*) FROM history_data GROUP BY history_uuid")
for r in cur.fetchall():
    print(r)
con.close()
