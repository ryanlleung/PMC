# KF16 right-angle pneumatic valve with 24 V solenoid pilot (GDQ-J16/KF)

11/11/26. A CadQuery model of the "L type 24 B" option of the AliExpress listing (item 1005012966628004): a GDQ-J16/KF right-angle pneumatic vacuum valve, B version (solenoid pilot fitted), 24 V DC coil. Rev 2 follows the listing's dimension table, drawing and photo.

## How it works
- The black block with the cable is the coil of the solenoid pilot valve, with a DIN plug on its side held by one centre screw.
- Coil on: the pilot lets the supply air (0.4 to 0.8 MPa) into the square cylinder, which lifts the bellows-sealed plate off the seat. The valve opens.
- Coil off: the pilot vents the cylinder through the two brass silencers and the spring closes the valve (normally closed).
- The coil comes off its stem after undoing the top nut and can go back facing any direction; the plug can be refitted in 90 degree steps. A plain DC coil is not polarity sensitive, but a plug with an LED or suppression diode is marked + and -. Neither changes normally closed.

## Listing data (GDQ-J16/KF)
- Range 1e5 to 1.3e-5 Pa, leak rate 1.3e-6 Pa L/s, -30 to +150 C, air 0.4 to 0.8 MPa, mounting any orientation.
- Coil AC 220 V default; AC 110 V, DC 24 V, DC 12 V to order. Magnetic switch position feedback to order (not modelled).

## Sizes

| Size | Value | Source |
| --- | --- | --- |
| DN, flange OD (phi D) | 16, 30 | listing table |
| phi B | 17.2, modelled as a 2.5 deep counterbore at each sealing face | listing table and drawing (my reading of "2.5") |
| Flange rim, clamp taper | 3 +/- 0.05, 15 deg +/- 10' | listing drawing |
| A, centre to each sealing face | 40 | listing table |
| H, axial port face to top of the pilot | 190 | listing table and drawing |
| Body | 28 dia, top 57 above the axial face | photo, ESTIMATED |
| Bonnet collar stack with label band | 48 dia, 28 tall incl. 44 square plate | photo, ESTIMATED |
| Square compact cylinder | 40 x 40, 47 tall, 22 dia boss on top | photo, ESTIMATED |
| Pilot valve | 18 x 22 on the cylinder's -X face, black solenoid section above it | photo, ESTIMATED |
| Coil, DIN plug | 28 x 28 x 28; plug 18 deep on the coil's +X side, gland down | photo, ESTIMATED |
| Air in | 6 mm push-in between two brass silencers, facing -Y | photo, ESTIMATED |

The photo sizes are scaled from the 30 mm flange and could be a few mm out. The pilot's side and the fitting direction follow the photo: if your valve has the pilot on another face, rotate those parts about Z.

## Files
- `kf16-angle-valve.step`: assembly with a named, coloured solid per part.
- `kf16-angle-valve.stl`: one solid.
- `renders/`: iso, back and front views.
- `src/kf16_angle_valve.py`: the model. `python3 src/kf16_angle_valve.py OUTDIR` also writes `OUTDIR/parts/*.stl`, one per part, named `part__colour.stl`.
- `src/render.py`: `python3 src/render.py OUTDIR/parts renders` (needs vtk; headless needs libosmesa6).

Coordinates: the port axes meet at the origin. Axial port faces -Z, side port faces +X, actuator up +Z.
