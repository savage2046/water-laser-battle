# -*- coding: utf-8 -*-
"""Dump JLC EDA Pro project (.eprj2 SQLite) structure."""
import sqlite3, sys, json

path = sys.argv[1] if len(sys.argv) > 1 else r'D:\ESP32\water-laser-battle\PCB\gun-board.eprj2'
con = sqlite3.connect(path)
con.text_factory = lambda b: b.decode('utf-8', 'replace')
cur = con.cursor()

print("=== TABLES ===")
cur.execute("SELECT name, type FROM sqlite_master WHERE type IN ('table','view') ORDER BY name")
tables = [r[0] for r in cur.fetchall()]
for name, typ in zip(tables, [r[1] for r in cur.execute("SELECT name, type FROM sqlite_master WHERE type IN ('table','view') ORDER BY name")]):
    print(f"{typ}: {name}")

for t in tables:
    try:
        cur.execute(f"SELECT COUNT(*) FROM '{t}'")
        cnt = cur.fetchone()[0]
        print(f"\n=== {t} ({cnt} rows) ===")
        cur.execute(f"PRAGMA table_info('{t}')")
        cols = [(c[1], c[2]) for c in cur.fetchall()]
        print("cols:", cols)
        if cnt > 0:
            cur.execute(f"SELECT * FROM '{t}' LIMIT 3")
            rows = cur.fetchall()
            for r in rows:
                s = []
                for v in r:
                    sv = str(v)
                    s.append(sv[:120] + ("..." if len(sv) > 120 else ""))
                print(" | ".join(s))
    except Exception as e:
        print(f"  (skip {t}: {e})")

print("\n\n=== FULL project_structures ===")
for r in cur.execute("SELECT id, ticket, structure FROM project_structures"):
    print(f"--- row id={r[0]} ticket={r[1]}")
    print(r[2])

print("\n\n=== FULL history_data (key/iv/uuid only + head) ===")
for r in cur.execute("SELECT id, uuid, history_uuid, length(dataStr), substr(dataStr,1,80) FROM history_data"):
    print(r)
con.close()
