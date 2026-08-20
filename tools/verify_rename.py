"""Final verification of project rename."""
import sqlite3
import os
import zipfile
import json
import re

con = sqlite3.connect("file:" + os.path.abspath(r"PCB\triger-sensor-current-test.eprj2").replace("\\", "/") + "?mode=ro", uri=True)
print("eprj2 projects.name:", con.execute("SELECT name FROM projects").fetchone()[0])
con.close()

z = zipfile.ZipFile(r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test_2026-08-19-18-40.fixed.epro2")
print("epro2 entries:", z.namelist())
print("project2 title:", json.loads(z.read("project2.json"))["title"])
data = z.read("triger-sensor-current-test.epru").decode("utf-8")
for ln in data.split("\n"):
    if "@Project Name" in ln and "e58fa6277ff7afab" in ln and '"value":"triger' in ln:
        m = re.search(r'"value":"([^"]+)"', ln)
        print("title-block @Project Name:", m.group(1))
        break
z.close()
