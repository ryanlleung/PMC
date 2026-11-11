"""KF16 right-angle ("L type") pneumatic vacuum valve with a 24 V solenoid pilot, CadQuery.

GDQ-J16/KF (B version: solenoid pilot fitted). The flange, A and H are from the
listing's dimension table and drawing. The other sizes are read off the listing
photo, scaled from the 30 mm flange, and are ESTIMATED.

Origin: where the two port axes meet. Axial port faces -Z, side port faces +X,
actuator up +Z, pilot on the -X face of the cylinder with its fittings facing -Y,
coil on top of the pilot with its DIN plug facing +Y (back) and the cable gland down.

    python3 kf16_angle_valve.py OUTDIR
"""
import math, os, sys
import cadquery as cq

# ---- from the listing (GDQ-J16/KF row of the dimension table, and the drawing)
KF_OD = 30.0      # phi D
KF_RECESS = 17.2  # phi B, bore at the sealing face (centring ring seat)
RECESS_DEPTH = 2.5  # "2.5" on the drawing, READ as the depth of phi B
KF_BORE = 16.0    # DN
KF_RIM = 3.0      # "3 +/- 0.05" on the drawing
KF_TAPER = 15.0   # "15 deg +/- 10'"
A = 40.0          # centre to each sealing face
H = 190.0         # axial port face to the top of the pilot

# ---- read off the listing photo, scaled from the 30 mm flange: ESTIMATED
TUBE_OD = 20.0                     # side port tube and the neck under the axial flange
BODY_D, BODY_TOP = 28.0, 17.0      # round stainless body, top above the origin
COLLAR_D = 48.0                    # bonnet collar stack with the label band
PLATE = 44.0                       # square plate under the cylinder
CYL_Z0, CYL_Z1 = 45.0, 92.0        # square compact air cylinder
CYL_W = 40.0
BOSS_D = 22.0
# pilot (5-port solenoid valve) on the -X face of the cylinder, coil on top
PIL_X0, PIL_X1 = -CYL_W / 2 - 22.0, -CYL_W / 2
PIL_Y = 11.0                       # half depth
PIL_Z0, PIL_Z1 = 48.0, 97.0
SOL_Z1 = 115.0                     # black solenoid tube section
COIL_Z0, COIL_Z1 = 115.0, 143.0    # black coil, 28 x 28
COIL_W = 24.0
COIL_XC = (PIL_X0 + PIL_X1) / 2    # coil on top of the pilot stack, same centre (drawing)
PLUG_D = 18.0                      # DIN plug on the coil's +Y face (to the back, hidden behind the coil in the
                                   # drawing's front view), cable gland pointing down (photo)
CABLE_D = 6.0


def kf_flange(face, direction):
    """Flange collar with its sealing face at `face`, facing `direction` (unit axis vector)."""
    ro, rt = KF_OD / 2, TUBE_OD / 2
    t2 = KF_RIM + (ro - rt) * math.tan(math.radians(KF_TAPER))
    pts = [(0, 0), (ro, 0), (ro, KF_RIM), (rt, t2), (0, t2)]
    s = cq.Workplane("XZ").polyline(pts).close().revolve(360, (0, 0, 0), (0, 1, 0)).val()
    d = cq.Vector(*direction)
    if d.z > 0.5:
        s = s.rotate((0, 0, 0), (1, 0, 0), 180)
    elif d.x > 0.5:
        s = s.rotate((0, 0, 0), (0, 1, 0), -90)
    return s.translate(cq.Vector(*face)), t2


def cyl(base, d, h, axis=(0, 0, 1)):
    return cq.Solid.makeCylinder(d / 2, h, cq.Vector(*base), cq.Vector(*axis))


def box(x0, x1, y0, y1, z0, z1):
    return cq.Solid.makeBox(x1 - x0, y1 - y0, z1 - z0, cq.Vector(x0, y0, z0))


def W(s):
    return cq.Workplane().add(s)


def build():
    parts = {}
    fa, t = kf_flange((0, 0, -A), (0, 0, -1))
    fs, _ = kf_flange((A, 0, 0), (1, 0, 0))
    body = (W(cyl((0, 0, -A + t - 0.1), TUBE_OD, 12))
            .union(W(cyl((0, 0, -A + t + 8), BODY_D, BODY_TOP + A - t - 8)).edges("<Z").chamfer(2.0))
            .union(W(cyl((0, 0, 0), TUBE_OD, A - t + 0.1, (1, 0, 0))))
            .union(W(fa)).union(W(fs)))
    bore = (W(cyl((0, 0, -A - 1), KF_BORE, A + 1))
            .union(W(cyl((0, 0, 0), KF_BORE, A + 1, (1, 0, 0))))
            .union(W(cyl((0, 0, -KF_BORE / 2), BODY_D - 6, KF_BORE / 2 + BODY_TOP - 3)))
            .union(W(cyl((0, 0, -A - 1), KF_RECESS, RECESS_DEPTH + 1)))
            .union(W(cyl((A + 1, 0, 0), KF_RECESS, RECESS_DEPTH + 1, (-1, 0, 0)))))
    parts["body"] = (body.cut(bore), "steel")
    # bonnet collar stack: lower disc, label band, upper disc, square plate
    parts["collar_low"] = (W(cyl((0, 0, BODY_TOP), COLLAR_D, 10)).edges().chamfer(0.6), "steel")
    parts["label_band"] = (W(cyl((0, 0, BODY_TOP + 10), COLLAR_D - 2, 10)), "label")
    parts["collar_up"] = (W(cyl((0, 0, BODY_TOP + 20), COLLAR_D, 4)).edges().chamfer(0.5), "steel")
    parts["plate"] = (W(box(-PLATE / 2, PLATE / 2, -PLATE / 2, PLATE / 2, BODY_TOP + 24, CYL_Z0)).edges("|Z").fillet(4.0), "alu_d")
    # square compact cylinder: corner bosses for the tie rods, a sensor slot on each face
    c = W(box(-CYL_W / 2, CYL_W / 2, -CYL_W / 2, CYL_W / 2, CYL_Z0, CYL_Z1)).edges("|Z").fillet(3.0)
    for (x, y, dx, dy) in ((CYL_W / 2, 0, 1, 0), (-CYL_W / 2, 0, 1, 0), (0, CYL_W / 2, 0, 1), (0, -CYL_W / 2, 0, 1)):
        w = 4.0
        sl = box(x - (1.2 if dx else w / 2), x + (1.2 if dx else w / 2), y - (1.2 if dy else w / 2), y + (1.2 if dy else w / 2), CYL_Z0 + 2, CYL_Z1 - 2)
        c = c.cut(W(sl))
    for sx in (-1, 1):
        for sy in (-1, 1):
            c = c.cut(W(cyl((sx * (CYL_W / 2 - 5), sy * (CYL_W / 2 - 5), CYL_Z1 - 3), 6.0, 3.1)))
    parts["cylinder"] = (c, "alu")
    parts["boss"] = (W(cyl((0, 0, CYL_Z1), BOSS_D, 2.0)), "alu_d")
    # pilot valve, solenoid tube section, coil, nut cap
    parts["pilot_body"] = (W(box(PIL_X0, PIL_X1, -PIL_Y, PIL_Y, PIL_Z0, PIL_Z1)).edges("|Z").fillet(1.0), "alu_d")
    parts["solenoid"] = (W(box(PIL_X0, PIL_X1, -PIL_Y, PIL_Y, PIL_Z1, SOL_Z1)).edges("|Z").fillet(1.5), "black")
    parts["coil"] = (W(box(COIL_XC - COIL_W / 2, COIL_XC + COIL_W / 2, -COIL_W / 2, COIL_W / 2, COIL_Z0, COIL_Z1)).edges("|Z").fillet(2.0), "black")
    parts["coil_cap"] = (W(cyl((COIL_XC, 0, COIL_Z1), 10.0, H - A - COIL_Z1)), "black2")
    py0 = COIL_W / 2
    parts["plug"] = (W(box(COIL_XC - COIL_W / 2 + 1, COIL_XC + COIL_W / 2 - 1, py0, py0 + PLUG_D, COIL_Z0 + 1, COIL_Z1 - 1)).edges("|Y").fillet(2.0), "black2")
    gy = py0 + PLUG_D / 2
    parts["gland"] = (W(cyl((COIL_XC, gy, COIL_Z0 + 1), 11.0, 9.0, (0, 0, -1))), "black2")
    g0 = COIL_Z0 - 8
    # cable drops from the gland and leads away to the back, clear of the cylinder (y <= 20) and plate
    path = cq.Workplane().spline([(COIL_XC, gy, g0), (COIL_XC, gy + 4, g0 - 15), (COIL_XC - 2, gy + 20, g0 - 40)],
                                 tangents=[(0, 0, -1), (0, 0.6, -1)], includeCurrent=False)
    parts["cable"] = (cq.Workplane(cq.Plane(origin=(COIL_XC, gy, g0), xDir=(1, 0, 0), normal=(0, 0, -1))).circle(CABLE_D / 2).sweep(path), "cable")
    # front (-Y) face of the pilot: two brass silencers and the 6 mm push-in air inlet between them
    xc = (PIL_X0 + PIL_X1) / 2
    for z in (62.0, 80.0):
        parts[f"silencer_{int(z)}"] = (W(cyl((xc + 3, -PIL_Y, z), 9.0, 6.0, (0, -1, 0))).union(W(cyl((xc + 3, -PIL_Y - 6, z), 7.0, 5.0, (0, -1, 0)))), "bronze")
    parts["air_in"] = (W(cyl((xc - 4, -PIL_Y, 71.0), 10.0, 14.0, (0, -1, 0))), "black2")
    parts["air_tube"] = (W(cyl((xc - 4, -PIL_Y - 14, 71.0), 6.0, 25.0, (0, -1, 0))), "tube")
    return parts


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(os.path.join(out, "parts"), exist_ok=True)
    parts = build()
    colours = {"steel": (0.78, 0.80, 0.83), "alu": (0.82, 0.83, 0.85), "alu_d": (0.70, 0.72, 0.75), "black": (0.08, 0.08, 0.09),
               "black2": (0.14, 0.14, 0.15), "cable": (0.12, 0.12, 0.12), "nickel": (0.75, 0.75, 0.72), "tube": (0.25, 0.45, 0.85),
               "bronze": (0.62, 0.52, 0.35), "label": (0.86, 0.86, 0.84)}
    asm = cq.Assembly(name="kf16_angle_valve")
    allp = None
    for n, (wp, c) in parts.items():
        asm.add(wp, name=n, color=cq.Color(*colours[c]))
        cq.exporters.export(wp, os.path.join(out, "parts", f"{n}__{c}.stl"), tolerance=0.05, angularTolerance=0.2)
        allp = wp if allp is None else allp.union(wp)
    asm.save(os.path.join(out, "kf16-angle-valve.step"))
    cq.exporters.export(allp, os.path.join(out, "kf16-angle-valve.stl"), tolerance=0.05, angularTolerance=0.2)
    bb = allp.val().BoundingBox()
    print("bbox x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f" % (bb.xmin, bb.xmax, bb.ymin, bb.ymax, bb.zmin, bb.zmax), "valid", allp.val().isValid())
