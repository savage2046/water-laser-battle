"""Find WIRE objects and their ids to locate the bypass wire."""
import sys
import zipfile
import re
sys.path.insert(0, "tools")

SRC = r"PCB\triger-sensor-current-test_2026-08-20.epro2"
z = zipfile.ZipFile(SRC)
name = [n for n in z.namelist() if n.endswith(".epru")][0]
data = z.read(name).decode("utf-8", errors="replace")
z.close()
lines = data.split("\n")

print("=== all WIRE objects ===")
for i, l in enumerate(lines):
    if '"type":"WIRE"' in l:
        m = re.search(r'"id":"([0-9a-f]+)"', l)
        print(f"line {i}: id={m.group(1) if m else '?'}  {l[:130]}")

print("\n=== LINE objects with lineGroup 5c5ff70e ===")
for i, l in enumerate(lines):
    if "5c5ff70e" in l:
        print(f"line {i}: {l[:200]}")
