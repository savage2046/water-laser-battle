"""Inspect raw structure: wire groups, tickets, placeholders."""
import sys
import zipfile
import json
import re

SRC = r"PCB\triger-sensor-current-test_backup\triger-sensor-current-test_2026-08-19-18-40.epro2"
z = zipfile.ZipFile(SRC)
data = z.read("triger-sensor-current-test.epru").decode("utf-8", errors="replace")
z.close()

lines = data.split("\n")
print("total lines:", len(lines))

# max ticket
max_t = 0
for ln in lines:
    m = re.search(r'"ticket":\s*(\d+)', ln)
    if m:
        max_t = max(max_t, int(m.group(1)))
print("max ticket:", max_t)

# locate SCH_PAGE document range
start = end = None
for i, ln in enumerate(lines):
    if '"docType":"SCH_PAGE"' in ln:
        start = i
    if start is not None and i > start and '"docType"' in ln and '"SCH_PAGE"' not in ln:
        end = i
        break
print(f"SCH_PAGE doc lines: {start} .. {end}")

# show raw lines of a complete wire group: WIRE 61c24bc2da41076b
target = "61c24bc2da41076b"
for i in range(start, end):
    if target in lines[i]:
        # print this and following lines until next WIRE/DOCHEAD (ATTRs follow)
        for j in range(i, min(i + 4, end)):
            print(f"[{j}] {lines[j][:300]}")
        print("---")

# placeholders
print("=== ELE_PLACEHOLDER rows ===")
for i in range(start, end):
    if '"ELE_PLACEHOLDER"' in lines[i]:
        print(lines[i][:220])
