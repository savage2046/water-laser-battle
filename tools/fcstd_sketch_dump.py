"""Concise dump of a FreeCAD document: feature tree + sketch geometry + key properties.

Usage: set env FC_FILE=<path> then run under freecadcmd with -c runpy.
"""
import os
import FreeCAD as App
import Part

path = os.environ["FC_FILE"]
doc = App.openDocument(path)
print(f"Document: {doc.Label}")

SKIP = ("App::Origin", "App::Line", "App::Plane", "App::Point",
        "App::DocumentObjectGroup")

def skip(o):
    return o.TypeId in SKIP or o.Name in ("Origin", "Origin001")

def fmt_plc(obj):
    try:
        p = obj.Placement
        return f"pos=({p.Base.x:.3f},{p.Base.y:.3f},{p.Base.z:.3f}) rot={p.Rotation.toEuler():.1f}"
    except Exception:
        return ""

def shape_info(obj):
    try:
        s = obj.Shape
        if s.isNull():
            return ""
        bb = s.BoundBox
        solids = len(s.Solids)
        return (f"bbox=({bb.XLength:.2f}x{bb.YLength:.2f}x{bb.ZLength:.2f}) "
                f"solid={solids} vol={s.Volume/1000:.2f}cm3")
    except Exception:
        return ""

def walk(obj, depth=0):
    if skip(obj):
        return
    ind = "  " * depth
    extra = shape_info(obj) or fmt_plc(obj)
    print(f"{ind}- {obj.Label} [{obj.Name}] :: {obj.TypeId}  {extra}")
    for ch in obj.OutList:
        walk(ch, depth + 1)

for o in doc.Objects:
    walk(o)

print("--- sketches ---")
def is_const(g):
    try:
        return bool(getattr(g, "Construction", False))
    except Exception:
        return False

for o in doc.Objects:
    if o.TypeId == "Sketcher::SketchObject" and not skip(o):
        print(f"\n[{o.Label}] Attachment: {o.AttachmentOffset}")
        for i, g in enumerate(o.Geometry):
            t = g.TypeId
            if "Circle" in t:
                c = g.Center
                print(f"  {i}: CIRCLE center=({c.x:.3f},{c.y:.3f}) r={g.Radius:.3f} {is_const(g) and '(const)' or ''}")
            elif "ArcOfCircle" in t:
                c = g.Center
                import math
                a1 = math.degrees(g.FirstParameter)
                a2 = math.degrees(g.LastParameter)
                print(f"  {i}: ARC center=({c.x:.3f},{c.y:.3f}) r={g.Radius:.3f} from={a1:.1f}to={a2:.1f}")
            elif "LineSegment" in t:
                s, e = g.StartPoint, g.EndPoint
                print(f"  {i}: LINE ({s.x:.3f},{s.y:.3f}) -> ({e.x:.3f},{e.y:.3f})")
            else:
                print(f"  {i}: {t}")
        consts = [c.TypeId for c in o.Constraints]
        if consts:
            print(f"  constraints: {len(consts)} -> {consts[:12]}")

doc.recompute()
App.closeDocument(doc.Name)
print("\ndone")
