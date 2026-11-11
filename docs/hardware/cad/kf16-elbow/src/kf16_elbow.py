"""Parametric ISO-KF DN16 (KF16) 90 degree elbow, CadQuery.

Corner of the two centrelines at the origin. Port A faces +X, port B faces +Z.
All sizes in mm. Sizes marked ASSUMED are not from the listing: caliper the part
and change them here.

    python3 kf16_elbow.py OUTDIR [LEG]    # LEG = centre to face, default 24
"""
import math
import sys
import os
import cadquery as cq

# ISO-KF DN16 flange
FLANGE_OD = 30.0      # ISO 2861 / DIN 28403 DN10/16
BORE = 16.0           # nominal bore (centring ring sleeve sits in it)
RIM = 3.0             # cylindrical rim at the flange OD, ASSUMED
TAPER_DEG = 15.0      # clamp face, 15 deg to the sealing face (ISO-KF)

# body
TUBE_OD = 19.0        # ASSUMED (19 x 1.5 tube is common on these elbows)
LEG = 24.0            # centre to sealing face, ASSUMED from "24" in the option name


def collar_len(tube_od=TUBE_OD):
    return RIM + (FLANGE_OD - tube_od) / 2 * math.tan(math.radians(TAPER_DEG))


def flange(leg, axis):
    """Flange collar revolved about its axis, sealing face at distance leg."""
    ro, rt = FLANGE_OD / 2, TUBE_OD / 2
    t1 = RIM
    t2 = collar_len()
    # profile in (r, t): t measured from the sealing face back toward the corner
    pts = [(0, 0), (ro, 0), (ro, t1), (rt, t2), (0, t2)]
    wp = cq.Workplane("XZ").polyline(pts).close().revolve(360, (0, 0, 0), (0, 1, 0))
    # revolved about local y (= global Z), t along +Z; now place the face at Z = leg
    s = wp.val().mirror("XY").translate((0, 0, leg))   # t now runs from leg down
    if axis == "x":
        s = s.rotate((0, 0, 0), (0, 1, 0), 90)          # +Z -> +X
    return s


def elbow(leg=LEG, bend_r=None):
    c = collar_len()
    if bend_r is None:
        bend_r = min(1.5 * TUBE_OD, leg - c - 1.0)
    if bend_r <= TUBE_OD / 2:
        raise ValueError("leg too short for a bent tube at this tube OD")
    r = bend_r
    k = r - r / math.sqrt(2)

    def path():
        return (cq.Workplane("XZ").moveTo(leg, 0).lineTo(r, 0)
                .threePointArc((k, k), (0, r)).lineTo(0, leg))

    outer = cq.Workplane("YZ", origin=(leg, 0, 0)).circle(TUBE_OD / 2).sweep(path())
    body = outer.union(cq.Workplane().add(flange(leg, "x"))).union(cq.Workplane().add(flange(leg, "z")))

    e = 1.0
    bore_path = (cq.Workplane("XZ").moveTo(leg + e, 0).lineTo(r, 0)
                 .threePointArc((k, k), (0, r)).lineTo(0, leg + e))
    bore = cq.Workplane("YZ", origin=(leg + e, 0, 0)).circle(BORE / 2).sweep(bore_path)
    body = body.cut(bore)
    return body, dict(leg=leg, bend_r=r, collar=c)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    leg = float(sys.argv[2]) if len(sys.argv) > 2 else LEG
    os.makedirs(out, exist_ok=True)
    body, info = elbow(leg)
    name = f"kf16-elbow-{int(leg)}"
    cq.exporters.export(body, os.path.join(out, name + ".step"))
    cq.exporters.export(body, os.path.join(out, name + ".stl"), tolerance=0.05, angularTolerance=0.2)
    v = body.val()
    bb = v.BoundingBox()
    print(name, info, "volume %.0f mm3" % v.Volume(), "mass 304 SS %.0f g" % (v.Volume() * 7.93e-3),
          "bbox %.1f x %.1f x %.1f" % (bb.xlen, bb.ylen, bb.zlen), "valid", v.isValid())
