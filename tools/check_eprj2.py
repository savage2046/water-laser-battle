"""Inspect the updated .eprj2: tables, history_data encryption."""
import sqlite3
import os
import base64
import zlib

DB = r"PCB\triger-sensor-current-test.eprj2"
con = sqlite3.connect("file:" + os.path.abspath(DB).replace("\\", "/") + "?mode=ro", uri=True)
cur = con.cursor()

cur.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")
print("tables:", [r[0] for r in cur.fetchall()])

print("\n=== history_data rows (by size) ===")
cur.execute("SELECT uuid, length(dataStr) FROM history_data ORDER BY length(dataStr) DESC LIMIT 12")
for r in cur.fetchall():
    print(r)

print("\n=== largest dataStr decode attempts ===")
cur.execute("SELECT dataStr FROM history_data ORDER BY length(dataStr) DESC LIMIT 1")
d = cur.fetchone()[0]
print("raw head:", repr(d[:120]))
b = base64.b64decode(d)
print("b64 len:", len(b), "head:", repr(b[:80]))
try:
    inf = zlib.decompress(b)
    print("zlib OK, len:", len(inf), "head:", repr(inf[:300]))
except Exception as e:
    print("zlib fail:", e)
    # maybe raw deflate or LZ4 or AES
    try:
        inf = zlib.decompress(b, -15)
        print("raw-deflate OK len:", len(inf), "head:", repr(inf[:300]))
    except Exception as e2:
        print("raw-deflate fail:", e2)

# also check any other table with content
print("\n=== tables with rows ===")
for t in ["documents", "boards", "schematics", "coppers", "components", "project_structures", "db_paths"]:
    try:
        n = cur.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
        print(t, n)
    except Exception as e:
        print(t, "err", e)
con.close()
