"""Probe a FreeCAD .FCStd document: object tree, placements, bounding boxes.

Usage: freecadcmd fcstd_probe.py <file.FCStd>
"""
import os
import sys
import FreeCAD as App
import Part

path = os.environ.get("FC_FILE")
if path is None:
    print("no FC_FILE env var given")
    sys.exit(1)

doc = App.openDocument(path)
print(f"Document: {doc.Label}  ({doc.FileName})")
print("=" * 70)


def bbox_str(obj):
    try:
        shp = obj.Shape
        if shp.isNull():
            return "empty"
        bb = shp.BoundBox
        return (f"X {bb.XMin:.2f}..{bb.XMax:.2f}  "
                f"Y {bb.YMin:.2f}..{bb.YMax:.2f}  "
                f"Z {bb.ZMin:.2f}..{bb.ZMax:.2f}  "
                f"(L {bb.XLength:.2f} W {bb.YLength:.2f} H {bb.ZLength:.2f})")
    except Exception as e:
        return f"n/a ({type(e).__name__})"


def walk(obj, depth=0):
    ind = "  " * depth
    try:
        if hasattr(obj, "Shape"):
            typ = obj.TypeId
            extra = bbox_str(obj)
        else:
            typ = obj.TypeId
            extra = ""
        label = getattr(obj, "Label", obj.Name)
        print(f"{ind}- {label} [{obj.Name}] :: {typ}  {extra}")
    except Exception as e:
        print(f"{ind}- ? {type(e).__name__}: {e}")
    for child in obj.OutList:
        walk(child, depth + 1)


for obj in doc.Objects:
    walk(obj)

print("=" * 70)
# List features of any PartDesign bodies / sketch hints
for obj in doc.Objects:
    if obj.TypeId in ("PartDesign::Body", "Part::Feature", "Sketcher::SketchObject"):
        pass

doc.recompute()
App.closeDocument(doc.Name)
print("done")
