"""Full detail: R1(shunt) component attrs + bypass wire objects."""
import sys
import zipfile
sys.path.insert(0, "tools")

SRC = r"PCB\triger-sensor-current-test_2026-08-20.epro2"
z = zipfile.ZipFile(SRC)
name = [n for n in z.namelist() if n.endswith(".epru")][0]
data = z.read(name).decode("utf-8", errors="replace")
z.close()
lines = data.split("\n")

print("=== R1 shunt component (parent of Device attr line 3782) ===")
# find the parent id on line 3782
import re
ln = lines[3782]
m = re.search(r'"parentId":"([0-9a-f]+)"', ln)
if m:
    pid = m.group(1)
    print("component id:", pid)
    print("line 3782 full:", ln)
    print("\n--- all ATTRs of this component ---")
    for i, l in enumerate(lines):
        if '"parentId":"%s"' % pid in l:
            key = re.search(r'"key":"([^"]+)"', l)
            val = re.search(r'"value":("?"?[^",]*"?)', l)
            print(f"  line {i}: key={key.group(1) if key else '?'} value={val.group(1) if val else '?'}")

print("\n=== bypass wire prefix search ===")
for i, l in enumerate(lines):
    if "5c5ff70e9c37303b" in l or ("5c5ff70e" in l and ('"type":"WIRE"' in l or '"lineGroup"' in l)):
        print(i, l[:180])
