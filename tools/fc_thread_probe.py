"""Probe v9: Part::Sweep with Sections/Spine - placement semantics."""
import FreeCAD as App
import Part

doc = App.newDocument("probe")

helix = doc.addObject("Part::Helix", "Helix")
helix.Pitch = 1.0
helix.Height = 3.0
helix.Radius = 11.7
helix.Angle = 360.0


def mk(name, base):
    w = Part.Wire(Part.makePolygon([
        App.Vector(base + 11.3865, 0, 0.0), App.Vector(base + 12.0, 0, 0.354),
        App.Vector(base + 12.0, 0, -0.354), App.Vector(base + 11.3865, 0, 0.0)]))
    f = doc.addObject("Part::Feature", name)
    f.Shape = Part.Face(w)
    return f


for label, base in (("onR", 0.0), ("at0", -11.7)):
    prof = mk("Prof_" + label, base)
    for frenet in (True, False):
        sw = doc.addObject("Part::Sweep", f"Sw_{label}_{frenet}")
        try:
            sw.Sections = [prof]
            sw.Spine = helix
            sw.Solid = True
            sw.Frenet = frenet
            doc.recompute()
            s = sw.Shape
            bb = s.BoundBox
            rmax = max(bb.XMax, -bb.XMin, bb.YMax, -bb.YMin)
            rmin = min(abs(bb.XMin), abs(bb.XMax), abs(bb.YMin), abs(bb.YMax))
            print(f"Sweep {label} frenet={int(frenet)}: vol={s.Volume:7.1f} "
                  f"radii[{rmin:6.2f},{rmax:6.2f}] Z[{bb.ZMin:5.2f},{bb.ZMax:5.2f}]")
        except Exception as e:
            print(f"Sweep {label} frenet={int(frenet)}: ERR {type(e).__name__}: {e}")
        finally:
            doc.removeObject(sw.Name)
    doc.removeObject(prof.Name)
