"""Rename internal project name to triger-sensor-current-test."""
import sqlite3
import os
import zipfile
import shutil

NEW = "triger-sensor-current-test"
EPRJ2 = r"PCB\triger-sensor-current-test.eprj2"
FIXED = r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test_2026-08-19-18-40.fixed.epro2"

# ---------- 1. .eprj2 (SQLite) ----------
bak = r"PCB\backup_before_fix\triger-sensor-current-test.orig.eprj2"
shutil.copy2(EPRJ2, bak)
con = sqlite3.connect(EPRJ2)
cur = con.cursor()
cur.execute("SELECT name FROM projects")
before = cur.fetchall()
cur.execute("UPDATE projects SET name=? WHERE name='triger-sensor'", (NEW,))
con.commit()
cur.execute("SELECT name FROM projects")
after = cur.fetchall()
con.close()
print("eprj2 projects.name:", before, "->", after, "| backup:", bak)

# ---------- 2. fixed .epro2 (project2.json title + epru entry name + @Project Name) ----------
z = zipfile.ZipFile(FIXED)
names = z.namelist()
epru_name = [n for n in names if n.endswith(".epru")][0]
epru_raw = z.read(epru_name).decode("utf-8", errors="replace")
proj2 = z.read("project2.json").decode("utf-8", errors="replace")
others = {n: z.read(n) for n in names if n not in (epru_name, "project2.json")}
z.close()

# project2.json title
import json
pj = json.loads(proj2)
pj["title"] = NEW
proj2_new = json.dumps(pj, ensure_ascii=False, indent=2)

# epru: title-block @Project Name attr on the SCH_PAGE instance
# (parentId = Drawing-Symbol_A4 component; the SYMBOL template attr has parentId="")
lines = epru_raw.split("\n")
hit = 0
for i, ln in enumerate(lines):
    if '"key":"@Project Name"' not in ln or '"parentId":"e58fa6277ff7afab"' not in ln:
        continue
    assert '"value":"triger-sensor"' in ln, f"line {i}: unexpected value"
    lines[i] = ln.replace('"value":"triger-sensor"', '"value":"%s"' % NEW, 1)
    hit += 1
    print(f"epru @Project Name updated on line {i}")
    break
assert hit == 1, "@Project Name attr not found"
epru_new = "\n".join(lines)

# repack with renamed epru entry
with zipfile.ZipFile(FIXED, "w", zipfile.ZIP_DEFLATED) as zout:
    for n in names:
        if n == epru_name:
            zout.writestr(NEW + ".epru", epru_new)
        elif n == "project2.json":
            zout.writestr(n, proj2_new)
        else:
            zout.writestr(n, others[n])
print("fixed.epro2 repacked; epru entry:", NEW + ".epru")
print("project2.json title:", NEW)
