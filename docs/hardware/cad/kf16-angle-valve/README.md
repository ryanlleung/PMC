# KF16 right-angle pneumatic valve with 24 V solenoid pilot

11/11/26. A CadQuery model of the "L type 24 B" option of the AliExpress listing (item 1005012966628004). The listing could not be read here, so the model follows the usual Chinese GDQ-J16(B) pattern: "L type" is the right-angle body, "B" the version with a solenoid pilot valve fitted, "24" a 24 V DC coil. Only the KF16 flange is a standard size. Everything else is ESTIMATED; change the values at the top of `src/kf16_angle_valve.py` once the part or the listing drawing is measured.

## How it works
- The black block with the cable is the coil of the 3/2 solenoid pilot valve. The cable ends in a DIN 43650 plug held to the coil by one centre screw.
- Coil on: the pilot lets the supply gas (typically 0.4 to 0.8 MPa) into the cylinder, which lifts the bellows-sealed plate off the seat. The valve opens.
- Coil off: the pilot vents the cylinder through the silencer and the spring closes the valve (normally closed).
- The coil comes off its stem after undoing the top nut and can go back facing any direction. The plug can be refitted in 90 degree steps. A plain DC coil is not polarity sensitive, but a plug with an LED or suppression diode is marked + and -. Neither changes normally closed to normally open, which is set by the spring.

## Sizes

| Size | Value | Source |
| --- | --- | --- |
| Flange OD, bore, clamp taper | 30, 16, 15 deg | ISO-KF DN16 standard |
| Flange rim, tube OD | 3.0, 19 | ASSUMED |
| Centre to sealing face, both ports | 40 | ESTIMATED (common for KF16 angle valves) |
| Axial port face to cylinder top | 118 | ESTIMATED (same as XV-1 in the GXC enclosure model) |
| Cylinder diameter | 46 (48 over the caps) | ESTIMATED |
| Body, bonnet collar, bellows housing | 30, 38, 26 dia | ESTIMATED |
| Pilot body | 40 x 18 x 16 on the cylinder cap | ESTIMATED; on some valves it sits on the side of the cylinder |
| Coil | 22 x 22 x 28, black | ESTIMATED |
| DIN plug | 27 x 27 x 18, cable 6 mm | ESTIMATED |
| Air inlet | 6 mm push-in, silencer beside it | ESTIMATED |

Overall height with the pilot and coil: about 166 from the axial port face.

## Files
- `kf16-angle-valve.step`: assembly with a named, coloured solid per part.
- `kf16-angle-valve.stl`: one solid.
- `renders/`: iso, back and front views.
- `src/kf16_angle_valve.py`: the model. `python3 src/kf16_angle_valve.py OUTDIR` also writes `OUTDIR/parts/*.stl`, one per part, named `part__colour.stl`.
- `src/render.py`: `python3 src/render.py OUTDIR/parts renders` (needs vtk; headless needs libosmesa6).

Coordinates: the port axes meet at the origin. Axial port faces -Z, side port faces +X, actuator up +Z.
