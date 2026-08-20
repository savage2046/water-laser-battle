"""Inspect the 10mOhm device doc raw structure and R1 attrs for the edit."""
import sys
import zipfile
import json
sys.path.insert(0, "tools")

SRC = r"PCB\triger-sensor-current-test_2026-08-20.epro2"
z = zipfile.ZipFile(SRC)
name = [n for n in z.namelist() if n.endswith(".epru")][0]
data = z.read(name).decode("utf-8", errors="replace")
z.close()
lines = data.split("\n")

# max ticket
import re
max_t = 0
for ln in lines:
    m = re.search(r'"ticket":\s*(\d+)', ln)
    if m:
        max_t = max(max_t, int(m.group(1)))
print("max ticket:", max_t)

# find DEVICE doc for RE2512F3R010 (uuid 26ef21da74600fef)
for i, ln in enumerate(lines):
    if '"docType":"DEVICE"' in ln and "26ef21da74600fef" in ln:
        print(f"\n--- DEVICE DOCHEAD line {i} ---")
        print(lines[i])
        print(f"--- META line {i+1} ---")
        print(lines[i+1][:2000])
        break

# find R1 (shunt) Device attr in SCH_PAGE
print("\n--- Device attrs referencing 26ef21da74600fef (10mOhm) ---")
for i, ln in enumerate(lines):
    if '"key":"Device"' in ln and "26ef21da74600fef" in ln:
        print(i, ln[:250])

# find the bypass wire and its ATTRs
print("\n--- bypass wire 5c5ff70e9c37303b objects ---")
for i, ln in enumerate(lines):
    if "5c5ff70e9c37303b" in ln:
        print(i, ln[:220])
