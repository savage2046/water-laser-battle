"""Read plaintext parts of updated .eprj2: structure, projects, thumbnail."""
import sqlite3
import os
import json

DB = r"PCB\triger-sensor-current-test.eprj2"
con = sqlite3.connect("file:" + os.path.abspath(DB).replace("\\", "/") + "?mode=ro", uri=True)
cur = con.cursor()

print("=== projects ===")
cur.execute("SELECT uuid, name, pcb_count, updated_at FROM projects")
for r in cur.fetchall():
    print(r)

print("\n=== project_structures (latest) ===")
cur.execute("SELECT id, substr(structure,1,4000) FROM project_structures ORDER BY id DESC LIMIT 1")
r = cur.fetchone()
if r:
    d = json.loads(r[1])
    print(json.dumps(d, ensure_ascii=False, indent=1))

print("\n=== backups table ===")
cur.execute("SELECT * FROM backups LIMIT 5")
cols = [c[0] for c in cur.description]
for r in cur.fetchall():
    print(dict(zip(cols, r)))

print("\n=== project_images (thumbnail size) ===")
cur.execute("SELECT uuid, length(image_data) FROM project_images")
for r in cur.fetchall():
    print(r)
con.close()
