"""Add an experiment-board notice to the schematic title block (Description)."""
import zipfile

SRC = r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test_2026-08-19-18-40.fixed.epro2"
NOTE = "实验/测试主板：电流变化前期研究用，后期实际PCB将重新设计"

z = zipfile.ZipFile(SRC)
names = z.namelist()
epru_name = [n for n in names if n.endswith(".epru")][0]
raw = z.read(epru_name).decode("utf-8", errors="replace")
others = {n: z.read(n) for n in names if n != epru_name}
z.close()

lines = raw.split("\n")
target_parent = "e58fa6277ff7afab"  # Drawing-Symbol_A4 component on SCH_PAGE
hit = 0
for i, ln in enumerate(lines):
    if '"key":"Description"' not in ln or '"parentId":"%s"' % target_parent not in ln:
        continue
    assert '"value":""' in ln, f"line {i}: expected empty Description value"
    lines[i] = ln.replace('"value":""', '"value":"%s"' % NOTE, 1)
    hit += 1
    print(f"Description set on line {i}")
    break
assert hit == 1, "Description attr not found"

new_epru = "\n".join(lines)
with zipfile.ZipFile(SRC, "w", zipfile.ZIP_DEFLATED) as zout:
    for n in names:
        if n == epru_name:
            zout.writestr(n, new_epru)
        else:
            zout.writestr(n, others[n])
print(f"repacked {len(names)} entries -> {SRC}")
