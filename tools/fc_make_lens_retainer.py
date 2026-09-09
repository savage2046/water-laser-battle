"""Build parametric FCStd v2: 20mm collimator lens retainer.

主体 (main body): rear internal M20x1 bore (onto gun-muzzle barrel thread),
                  front chamber for lens, front exterior M24x1 thread zone.
前压盖 (threaded cap, OD > body): internal M24x1 thread + window lid.
                  Screws over the body front; inner step clamps the lens rim
                  against the chamber seat (lens front flush with body front).

Axis: +Z forward (muzzle / light-out). z=0 = rear.
Tooth flanks are NOT modelled (v2): interfaces are given as ISO diameters so
threads can be added later or tapped/printed.  All parts are editable
Part primitives + booleans.

Usage:  freecadcmd -c "import runpy; runpy.run_path(r'<this file>', run_name='__main__')"
"""
import FreeCAD as App
import Part

OUT = r"D:\ESP32\water-laser-battle\case\20准直镜头压环.FCStd"

# ----------------------------------------------------------------------
# Parameters (mm)
# ----------------------------------------------------------------------
P = dict(
    body_od=24.0,          # 主体外径 (≈旧安装架口径, 前段为 M24x1 螺纹区)
    body_len=15.0,         # 主体总长 = 攻丝10 + 镜头舱5 (z 0..15)
    m20_minor=18.92,       # M20x1 内螺纹小径 = 后孔攻丝直径 (z 0..10)
    thread_len=10.0,       # 后段 M20x1 攻丝长度
    chamber_bore=20.6,     # 镜头舱孔径 (镜片Φ20 + 0.3 间隙/侧)
    chamber_depth=5.0,     # 舱深 = 镜厚 → 镜片前缘与主体端面齐平
    lens_od=20.0,
    lens_t=5.0,
    window=17.6,           # 出光孔 (内檐 1.2mm 压镜片缘)
    front_thread="M24x1",  # 主体前端外螺纹(概念区 z9..15, 未建模牙形)
    front_thread_len=6.0,  # 外螺纹区长度
    cap_od=28.0,           # 压盖外径 > 主体 (Φ28)
    cap_bore=22.92,        # M24x1 内螺纹小径 (压盖内孔, 啮合主体Φ24)
    cap_lid_len=1.5,       # 压盖前端盖厚 (z15..16.5, 出光孔在此)
)
R = {k: v / 2.0 for k, v in P.items() if isinstance(v, (int, float))}

doc = App.newDocument("20mm准直镜头压环")
doc.Label = "20mm准直镜头压环 v2 (旋盖式)"


def cyl(name, r, z0, h, label=None, color=None):
    o = doc.addObject("Part::Cylinder", name)
    o.Label = label or name
    o.Radius = r
    o.Height = h
    o.Placement.Base = App.Vector(0, 0, z0)
    if color:
        try:
            o.ViewObject.ShapeColor = color
        except AttributeError:
            pass
    return o


def cut(base, tool, name, label):
    o = doc.addObject("Part::Cut", name)
    o.Label = label
    o.Base = base
    o.Tool = tool
    return o


# ======================================================================
# 主体  z0..15: 后孔M20x1(0..10) | 镜头舱Φ20.6(10..15) | 前端M24x1外螺纹概念区(9..15)
# ======================================================================
outer = cyl("main_outer", R["body_od"], 0, P["body_len"], "主体_外径Φ24",
            (0.75, 0.75, 0.78))
bore = cyl("rear_bore", R["m20_minor"], 0, P["thread_len"],
           "后孔_M20x1_攻丝预留_Φ18.92x10")
cham = cyl("chamber", R["chamber_bore"], P["thread_len"], P["chamber_depth"],
           f"镜头舱_Φ{P['chamber_bore']}x{P['chamber_depth']}")

body_cut1 = cut(outer, bore, "Body_cut_thread", "主体_去后孔")
body = cut(body_cut1, cham, "Body_cut_chamber", "主体(镜头座, 前端M24x1外螺纹区)")

# ======================================================================
# 前压盖  帽形: 内螺纹区 z9..15 (啮合主体), 端盖 z15..16.5 带Φ17.6出光孔
# ======================================================================
ZT = P["body_len"] - P["front_thread_len"]          # 螺纹区起点 z=9
cap_outer = cyl("cap_outer", R["cap_od"], ZT,
                P["front_thread_len"] + P["cap_lid_len"],   # 高7.5
                f"压盖_外径Φ{P['cap_od']}", (0.55, 0.62, 0.78))
cap_bore = cyl("cap_inner_thread", R["cap_bore"], ZT, P["front_thread_len"],
               f"压盖_内孔_M24x1小径Φ{P['cap_bore']}")
cap_win = cyl("cap_window", R["window"], P["body_len"], P["cap_lid_len"],
              f"出光孔_Φ{P['window']}")

cap_cut1 = cut(cap_outer, cap_bore, "Cap_cut_bore", "压盖_车内螺纹孔")
cap = cut(cap_cut1, cap_win, "Cap_cut_window",
          f"前压盖(内M24x1, Φ{P['window']}出光, 外径Φ{P['cap_od']})")

# ======================================================================
# 参考体: 镜片 Φ20x5 (z10..15, 前缘与主体端面 z15 齐平)
# ======================================================================
lens = cyl("ref_lens", R["lens_od"], P["thread_len"], P["lens_t"],
           f"参考_镜片Φ{P['lens_od']}x{P['lens_t']}", (0.9, 0.3, 0.3))
try:
    lens.ViewObject.Transparency = 40
except AttributeError:
    pass

# ======================================================================
# Spreadsheets: 参数表 + 装配说明
# ======================================================================
ss = doc.addObject("Spreadsheet::Sheet", "参数表")
ss.Label = "参数表(改这里对照)"
rows = [
    ("A1", "参数", "B1", "值mm", "C1", "说明"),
    ("A2", "body_od", "B2", P["body_od"], "C2", "主体外径(前段车 M24x1 外螺纹)"),
    ("A3", "body_len", "B3", P["body_len"], "C3", "主体总长 = 攻丝10 + 舱5"),
    ("A4", "m20_minor", "B4", P["m20_minor"], "C4", "后孔 M20x1 攻丝直径(旋枪口外螺纹)"),
    ("A5", "thread_len", "B5", P["thread_len"], "C5", "后段 M20x1 攻丝长度"),
    ("A6", "chamber_bore", "B6", P["chamber_bore"], "C6", "镜头舱孔径 = 20 + 0.3/侧"),
    ("A7", "chamber_depth", "B7", P["chamber_depth"], "C7", "舱深 = 镜厚5 → 镜片端面齐平"),
    ("A8", "lens_od", "B8", P["lens_od"], "C8", "镜片外径"),
    ("A9", "lens_t", "B9", P["lens_t"], "C9", "镜片厚度"),
    ("A10", "window", "B10", P["window"], "C10", "压盖出光孔(内檐1.2mm压镜片缘)"),
    ("A11", "front_thread", "B11", P["front_thread"], "C11", "主体前端外螺纹规格"),
    ("A12", "front_thread_len", "B12", P["front_thread_len"], "C12", "外螺纹区长度 z9..15"),
    ("A13", "cap_od", "B13", P["cap_od"], "C13", "压盖外径 (>主体 24, 便于手拧/铣六角)"),
    ("A14", "cap_bore", "B14", P["cap_bore"], "C14", "压盖内孔 M24x1 小径(啮合主体Φ24)"),
    ("A15", "cap_lid_len", "B15", P["cap_lid_len"], "C15", "压盖端盖厚(出光孔长度)"),
]
for c1, v1, c2, v2, c3, v3 in rows:
    ss.set(c1, v1)
    ss.set(c2, str(v2))
    ss.set(c3, v3)

notes = doc.addObject("Spreadsheet::Sheet", "装配说明")
notes.Label = "装配说明"
nrows = [
    ("A1", "1) 枪口外筒需有 M20x1 外螺纹(牙顶≈Φ20): 主体后孔Φ18.92攻丝后旋入10mm。"),
    ("A2", "2) 镜片 Φ20x5 从前方放入镜头舱, 落到 z10 台阶, 前缘与主体端面 z15 齐平。"),
    ("A3", "3) 压盖(M24x1 内螺纹, 外径Φ28)从前方旋到主体前端外螺纹区(z9..15)。"),
    ("A4", "4) 旋到底: 压盖内台阶面(z15)压住镜片外缘(Φ17.6..20 环带), 主体端面为限位。"),
    ("A5", "5) 牙形未建模(v2 只给 ISO 直径): 主体外径Φ24/压盖内孔Φ22.92 为 M24x1 啮合区,"),
    ("A6", "   剖面里两件 0.5mm 径向重叠即螺纹齿区示意。需直接打印牙形请说, 下版加 Helix 齿。"),
    ("A7", "6) 打印方向建议: 主体轴线竖放(出光端朝上); 压盖轴线竖放、端盖(出光孔面)朝上,"),
    ("A8", "   端盖外圈Φ24→28 悬空 2mm 需加支撑或桥接。壁厚: 主体 1.7, 压盖 2.5+。"),
]
for c, v in nrows:
    notes.set(c, v)

doc.recompute()
App.setActiveDocument(doc.Name)

# ----------------------------------------------------------------------
# Verify
# ----------------------------------------------------------------------
def info(label, obj):
    s = obj.Shape
    bb = s.BoundBox
    print(f"{label}: vol={s.Volume / 1000:.3f} cm3  "
          f"bbox Z[{bb.ZMin:.1f},{bb.ZMax:.1f}]  Ø{bb.XLength:.1f}  "
          f"solid={len(s.Solids)} valid={s.isValid()}")

info("主体", body)
info("压盖", cap)
info("镜片参考", lens)

assert len(body.Shape.Solids) == 1
assert len(cap.Shape.Solids) == 1

def common_vol(a, b):
    try:
        return a.Shape.common(b.Shape).Volume
    except Exception as e:
        return f"err {e}"

objs = {"主体": body, "压盖": cap, "镜片": lens}
# 压盖×主体 允许 ~π(12²-11.46²)×6 ≈ 239mm³ 的重叠 = M24x1 螺纹啮合区(牙形未建模)
for combo in (("主体", "镜片"), ("压盖", "镜片"), ("压盖", "主体")):
    v = common_vol(objs[combo[0]], objs[combo[1]])
    if isinstance(v, float):
        if v <= 0.001:
            flag = "OK(零穿透)"
        elif combo == ("压盖", "主体") and 200 < v < 300:
            flag = f"OK(螺纹啮合区 {v:.0f}mm3, 牙形未建模)"
        else:
            flag = f"!! {v:.1f} mm3"
    else:
        flag = str(v)
    print(f"干涉 {'×'.join(combo)}: {flag}")

doc.saveAs(OUT)
print(f"saved -> {OUT}")

try:
    import Mesh
    for nm, obj, fn in (("body", body, "20准直镜头压环_主体"),
                        ("cap", cap, "20准直镜头压环_前压盖")):
        Mesh.export([obj], rf"D:\ESP32\water-laser-battle\case\{fn}.stl")
    print("stl previews exported")
except Exception as e:
    print(f"stl export skipped: {e}")
