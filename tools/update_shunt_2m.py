"""Update schematic: shunt resistor 10mOhm -> 2mOhm (C2994640) and
remove the bypass line that shorts the R4 buffer resistor."""
import zipfile
import uuid
import os
import re

SRC = r"PCB\triger-sensor-current-test_2026-08-20.epro2"
OUT = r"PCB\triger-sensor-current-test_fixed\triger-sensor-current-test_2026-08-20.fixed.epro2"

NEW_UUID = uuid.uuid4().hex[:16]
NEW_DEV = "HoLLR2512-3W-2mR-1%"

z = zipfile.ZipFile(SRC)
names = z.namelist()
epru_name = [n for n in names if n.endswith(".epru")][0]
raw = z.read(epru_name).decode("utf-8", errors="replace")
others = {n: z.read(n) for n in names if n != epru_name}
z.close()

lines = raw.split("\n")

# ---------- 1. new DEVICE document (2mOhm) inserted after RE2512 META (line 3434) ----------
old_meta = lines[3434]
m_ticket = re.search(r'"ticket":(\d+)', lines[3433]).group(1)
assert '"26ef21da74600fef"' in lines[3433] and '"title":"RE2512F3R010"' in old_meta

new_dochead = (
    '{"type":"DOCHEAD","ticket":6803}||{"docType":"DEVICE","client":"da7e295d4bfc9a4e",'
    '"uuid":"%s","updateTime":1787195818585,"version":"1787195818585",'
    '"editVersion":"3.2.184","user":{"uuid":"%s"}}|' % (NEW_UUID, NEW_UUID)
)
new_meta = (
    '{"type":"META","ticket":6804,"id":"META"}||{"title":"HoLLR2512-3W-2mR-1%",'
    '"tags":["电阻","电流采样电阻/分流器"],'
    '"source":"f720c2729e3b4438b4859713741cde08|0819f05c4eef4c71ace90d822a990e87",'
    '"images":["https://alimg.szlcsc.com/upload/public/product/middle/20230106/2C4BD9B300E693B02371F89B2E9C1EA3.jpg"],'
    '"attributes":{"LCSC Part Name":"2mΩ 3W 2512 ±1%","Supplier Part":"C2994640",'
    '"Manufacturer":"Milliohm(毫欧)","Manufacturer Part":"HoLLR2512-3W-2mR-1%",'
    '"Supplier Footprint":"2512","JLCPCB Part Class":"Extended Part",'
    '"Datasheet":"https://item.szlcsc.com/3460474.html","Supplier":"LCSC",'
    '"Add into BOM":"yes","Convert to PCB":"yes","Symbol":"0d9817cfdc71efdb",'
    '"Designator":"R?","Footprint":"4b2bf5d50e34729a",'
    '"3D Model":"954b67263d9e4f3582d8de3f8c846421|0819f05c4eef4c71ace90d822a990e87",'
    '"3D Model Title":"R2512_L6.3-W3.2-H0.6",'
    '"3D Model Transform":"248.031,125.984,0,0,0,0,-0.0005,0,0",'
    '"Type":"采样电阻；合金电阻","Value":"2mΩ","Tolerance":"±1%","Power(Watts)":"3W",'
    '"Temperature Coefficient":"±50ppm/℃","Name":"={Value}",'
    '"Description":"电阻类型:采样电阻；合金电阻;阻值:2mΩ;精度:±1%;功率:3W;温度系数:±50ppm/℃;"}}|'
)
lines[3434:3435] = [old_meta, new_dochead, new_meta]
print("new DEVICE", NEW_UUID, "inserted after line 3434")

# ---------- 2. R1 (shunt) component attrs (locate by content) ----------
def find_line(substr):
    for i, ln in enumerate(lines):
        if substr in ln:
            return i
    raise RuntimeError(f"line not found: {substr}")

def replace_in_line(idx, old, new, what):
    assert old in lines[idx], f"line {idx}: {old!r} not found for {what}"
    lines[idx] = lines[idx].replace(old, new, 1)
    print(f"line {idx}: {what}")

replace_in_line(find_line('"parentId":"5491cb72b8fa020e","zIndex":14'), '"value":"26ef21da74600fef"',
                '"value":"%s"' % NEW_UUID, "Device uuid")
replace_in_line(find_line('"key":"LCSC Part Name","fillColor":null,"parentId":"5491cb72b8fa020e"'),
                '"value":"10mΩ 3W 2512 ±1%"', '"value":"2mΩ 3W 2512 ±1%"', "LCSC Part Name")
replace_in_line(find_line('"key":"Supplier Part","fillColor":null,"parentId":"5491cb72b8fa020e"'),
                '"value":"C46961753"', '"value":"C2994640"', "Supplier Part")
replace_in_line(find_line('"key":"Manufacturer","fillColor":null,"parentId":"5491cb72b8fa020e"'),
                '"value":"JIERR(捷而瑞)"', '"value":"Milliohm(毫欧)"', "Manufacturer")
replace_in_line(find_line('"key":"Manufacturer Part","fillColor":null,"parentId":"5491cb72b8fa020e"'),
                '"value":"RE2512F3R010"', '"value":"HoLLR2512-3W-2mR-1%"', "Manufacturer Part")
replace_in_line(find_line('"key":"Datasheet","fillColor":null,"parentId":"5491cb72b8fa020e"'),
                '"value":"https://item.szlcsc.com/datasheet/RE2512F3R010/49133292.html"',
                '"value":"https://item.szlcsc.com/3460474.html"', "Datasheet")
replace_in_line(find_line('"key":"Value","fillColor":null,"parentId":"5491cb72b8fa020e"'),
                '"value":"10mΩ"', '"value":"2mΩ"', "Value")
replace_in_line(find_line('"key":"Description","fillColor":null,"parentId":"5491cb72b8fa020e"'),
                '"value":"电阻类型:采样电阻；合金电阻;阻值:10mΩ;精度:±1%;功率:3W;温度系数:±50ppm/℃;"',
                '"value":"电阻类型:采样电阻；合金电阻;阻值:2mΩ;精度:±1%;功率:3W;温度系数:±50ppm/℃;"', "Description")

# ---------- 3. remove bypass line that shorts R4 ----------
bypass_line = None
for i, ln in enumerate(lines):
    if '"lineGroup":"5c5ff70e9c37303b"' in ln and '"startX":585' in ln and '"endX":660' in ln:
        bypass_line = i
        break
assert bypass_line is not None, "bypass LINE not found"
del lines[bypass_line]
print(f"line {bypass_line}: bypass LINE removed (R4 buffer now in series)")

# ---------- write ----------
os.makedirs(os.path.dirname(OUT), exist_ok=True)
new_epru = "\n".join(lines)
with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as zout:
    for n in names:
        if n == epru_name:
            zout.writestr(n, new_epru)
        else:
            zout.writestr(n, others[n])
print("wrote", OUT)
