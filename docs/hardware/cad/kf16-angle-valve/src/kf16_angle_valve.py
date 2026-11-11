"""KF16 right-angle ("L type") pneumatic vacuum valve with a 24 V solenoid pilot, CadQuery.

Taken as the usual Chinese GDQ-J16(B) pattern: stainless angle body with two
ISO-KF DN16 ports, bellows bonnet, single-acting air cylinder (spring closes,
air opens) and a small 3/2 solenoid pilot valve on top, whose black coil takes
a DIN plug and cable. Only the KF16 flange is a standard size; every other size
is ESTIMATED until the part or the listing drawing is measured.

Origin: where the two port axes meet. Axial port faces -Z, side port faces +X,
actuator up +Z.

    python3 kf16_angle_valve.py OUTDIR
"""
import math, os, sys
import cadquery as cq

# ---- ISO-KF DN16 flange (standard)
KF_OD, KF_BORE, KF_RIM, KF_TAPER = 30.0, 16.0, 3.0, 15.0   # RIM ASSUMED
TUBE_OD = 19.0                                              # ASSUMED

# ---- valve, all ESTIMATED
A = 40.0          # centre to sealing face, both ports
H = 118.0         # axial port face to top of the cylinder
BODY_D = 30.0     # angle body (vertical cylinder)
BODY_TOP = 12.0
BONNET_D, BONNET_H = 38.0, 6.0      # bonnet clamp collar
NECK_D = 26.0                        # bellows housing
CYL_D = 46.0                         # air cylinder (anodised aluminium)
CYL_Z0 = 40.0
CYL_TOP = H - A                      # = 78 above the origin
# pilot valve (3/2 solenoid, 24 V DC), lying on the cylinder cap
PIL_L, PIL_W, PIL_H = 40.0, 18.0, 16.0
COIL = (22.0, 22.0, 28.0)            # black coil, square body
PLUG = (27.0, 27.0, 18.0)            # DIN 43650 form B (industrial) plug, black
CABLE_D = 6.0


def kf_flange(face, direction):
    """Flange collar with its sealing face at `face`, facing `direction` (unit axis vector)."""
    ro, rt = KF_OD / 2, TUBE_OD / 2
    t2 = KF_RIM + (ro - rt) * math.tan(math.radians(KF_TAPER))
    pts = [(0, 0), (ro, 0), (ro, KF_RIM), (rt, t2), (0, t2)]
    s = cq.Workplane("XZ").polyline(pts).close().revolve(360, (0, 0, 0), (0, 1, 0)).val()
    # built facing -Z (t runs +Z from the face). Turn to face `direction`.
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


def build():
    parts = {}
    # body: vertical cylinder plus the side branch
    fa, t = kf_flange((0, 0, -A), (0, 0, -1))
    fs, _ = kf_flange((A, 0, 0), (1, 0, 0))
    body = (cq.Workplane().add(cyl((0, 0, -A + t - 0.1), TUBE_OD, A - t + 0.1 - 6))
            .union(cq.Workplane().add(cyl((0, 0, -9), BODY_D, BODY_TOP + 9)))
            .union(cq.Workplane().add(cyl((0, 0, 0), TUBE_OD, A - t + 0.1, (1, 0, 0))))
            .union(cq.Workplane().add(fa)).union(cq.Workplane().add(fs)))
    bore = (cq.Workplane().add(cyl((0, 0, -A - 1), KF_BORE, A + 1))
            .union(cq.Workplane().add(cyl((0, 0, 0), KF_BORE, A + 1, (1, 0, 0))))
            .union(cq.Workplane().add(cyl((0, 0, -KF_BORE / 2), BODY_D - 6, KF_BORE / 2 + BODY_TOP - 3))))
    parts["body"] = (body.cut(bore), "steel")
    # bonnet collar, bellows housing
    parts["bonnet"] = (cq.Workplane().add(cyl((0, 0, BODY_TOP), BONNET_D, BONNET_H)).edges().chamfer(0.8), "steel")
    parts["neck"] = (cq.Workplane().add(cyl((0, 0, BODY_TOP + BONNET_H), NECK_D, CYL_Z0 - BODY_TOP - BONNET_H)), "steel")
    # air cylinder with end caps
    parts["cylinder"] = (cq.Workplane().add(cyl((0, 0, CYL_Z0 + 3), CYL_D, CYL_TOP - CYL_Z0 - 6)), "alu")
    parts["cap_low"] = (cq.Workplane().add(cyl((0, 0, CYL_Z0), CYL_D + 2, 3)).edges().chamfer(0.6), "alu_d")
    parts["cap_top"] = (cq.Workplane().add(cyl((0, 0, CYL_TOP - 3), CYL_D + 2, 3)).edges().chamfer(0.6), "alu_d")
    # pilot valve body on the cap (long axis along Y), coil standing up at the +Y end
    z0 = CYL_TOP
    parts["pilot_body"] = (cq.Workplane().add(box(-PIL_W / 2, PIL_W / 2, -PIL_L / 2, PIL_L / 2, z0, z0 + PIL_H)).edges("|Z").fillet(1.5), "alu_d")
    cy = PIL_L / 2 - COIL[1] / 2
    zc = z0 + PIL_H
    parts["armature_nut"] = (cq.Workplane().add(cyl((0, cy, zc + COIL[2]), 8.0, 4.0)), "steel")
    coil = cq.Workplane().add(box(-COIL[0] / 2, COIL[0] / 2, cy - COIL[1] / 2, cy + COIL[1] / 2, zc, zc + COIL[2])).edges("|Z").fillet(2.0)
    parts["coil"] = (coil, "black")
    # DIN plug on the coil's -X face, cable out of its gland downward... sideways (-X)
    px1 = -COIL[0] / 2
    px0 = px1 - PLUG[2]
    pz0 = zc + COIL[2] / 2 - PLUG[1] / 2 + 2
    plug = cq.Workplane().add(box(px0, px1, cy - PLUG[0] / 2, cy + PLUG[0] / 2, pz0, pz0 + PLUG[1])).edges("|X").fillet(2.0)
    parts["plug"] = (plug, "black2")
    gz = pz0 + PLUG[1] / 2
    parts["gland"] = (cq.Workplane().add(cyl((px0, cy, gz), 11.0, 8.0, (-1, 0, 0))), "black2")
    path = cq.Workplane().spline([(px0 - 8, cy, gz), (px0 - 35, cy + 5, gz - 15), (px0 - 50, cy + 10, gz - 70)],
                                 tangents=[(-1, 0, 0), (0, 0, -1)], includeCurrent=False)
    cable = cq.Workplane(cq.Plane(origin=(px0 - 8, cy, gz), xDir=(0, 1, 0), normal=(-1, 0, 0))).circle(CABLE_D / 2).sweep(path)
    parts["cable"] = (cable, "cable")
    # air supply push-in fitting (6 mm) and exhaust silencer on the -Y end of the pilot
    parts["air_in"] = (cq.Workplane().add(cyl((-4, -PIL_L / 2, z0 + PIL_H / 2), 10.0, 12.0, (0, -1, 0))), "nickel")
    parts["air_tube"] = (cq.Workplane().add(cyl((-4, -PIL_L / 2 - 12, z0 + PIL_H / 2), 6.0, 25.0, (0, -1, 0))), "tube")
    parts["silencer"] = (cq.Workplane().add(cyl((5, -PIL_L / 2, z0 + PIL_H / 2 - 2), 7.0, 10.0, (0, -1, 0))), "bronze")
    return parts


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(os.path.join(out, "parts"), exist_ok=True)
    parts = build()
    colours = {"steel": (0.78, 0.80, 0.83), "alu": (0.82, 0.83, 0.85), "alu_d": (0.70, 0.72, 0.75), "black": (0.08, 0.08, 0.09),
               "black2": (0.14, 0.14, 0.15), "cable": (0.12, 0.12, 0.12), "nickel": (0.75, 0.75, 0.72), "tube": (0.25, 0.45, 0.85),
               "bronze": (0.62, 0.52, 0.35)}
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
