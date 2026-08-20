"""Inspect a JLCEDA Pro (.eprj2) SQLite project database read-only."""
import sqlite3
import sys
import os

def main(path: str):
    uri = "file:" + os.path.abspath(path).replace("\\", "/") + "?mode=ro"
    con = sqlite3.connect(uri, uri=True)
    cur = con.cursor()
    cur.execute("SELECT name, type FROM sqlite_master WHERE type IN ('table','view') ORDER BY name")
    print("=== objects ===")
    for row in cur.fetchall():
        print(row)
    # For each table, print row count and first row keys
    cur.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")
    for (t,) in cur.fetchall():
        try:
            n = cur.execute(f'SELECT COUNT(*) FROM "{t}"').fetchone()[0]
            print(f"\n=== table {t}: {n} rows ===")
            cur.execute(f'SELECT * FROM "{t}" LIMIT 1')
            cols = [d[0] for d in cur.description]
            row = cur.fetchone()
            if row:
                for c, v in zip(cols, row):
                    s = str(v)
                    print(f"  {c}: {s[:300]}")
        except Exception as e:
            print(f"  (error: {e})")
    con.close()

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"PCB\triger-sensor-current-test.eprj2")
