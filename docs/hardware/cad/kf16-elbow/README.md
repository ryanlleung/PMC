# KF16 90 degree elbow

11/11/26. A CadQuery model of an ISO-KF DN16 (KF16) 90 degree elbow, for the "L type 24 B" option of the AliExpress listing (item 1005012966628004). The listing could not be read here, so every size below except the flange OD is a standard figure or an estimate. Caliper the part, or send the listing's dimension picture, and change the values at the top of `src/kf16_elbow.py`.

## Sizes

| Size | Value | Source |
| --- | --- | --- |
| Flange OD | 30.0 | ISO-KF DN10/16 standard |
| Bore | 16.0 | KF16 nominal (centring ring sleeve sits in it) |
| Clamp taper | 15 deg to the sealing face | ISO-KF standard |
| Flange rim (cylindrical part at the OD) | 3.0 | ASSUMED |
| Flange collar, face to tube | 4.47 | follows from the three above |
| Tube OD | 19.0 (19 x 1.5) | ASSUMED |
| Centre to sealing face, both legs | 24 | ASSUMED from "24" in the option name |
| Bend radius on the centreline | 18.5 | largest that leaves a straight stub behind each collar |

A 40 mm centre to face version is included as well, because 40 mm is the usual catalogue size for a bent KF16 elbow (bend radius 28.5).

With a 24 mm leg the inside of the bend has only a 9 mm radius, so the real part may be a machined block or a mitred elbow rather than a bent tube. The ports and flanges are the same either way; only the outside of the body changes.

Mass in 304 stainless: 51 g (24 mm), 69 g (40 mm), from the model.

## Files
- `kf16-elbow-24.step`, `kf16-elbow-24.stl`: centre to face 24.
- `kf16-elbow-40.step`, `kf16-elbow-40.stl`: centre to face 40.
- `renders/`: shaded views and a dimensioned section on the centrelines for each.
- `src/kf16_elbow.py`: the parametric model. `python3 src/kf16_elbow.py OUTDIR LEG`.
- `src/render.py`: renders. `python3 src/render.py kf16-elbow-24.stl 24 renders` (needs vtk and matplotlib; headless needs libosmesa6).

Coordinates: the two centrelines meet at the origin. Port A faces +X, port B faces +Z.
