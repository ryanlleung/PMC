# HMI template preview

The simulator with the common HMI template changes applied, for a look before any firmware change. The firmware in `InitialTesting` is not changed: `build.sh` copies it, applies `hmi-template.patch` to the copy and compiles that with `simulator/wasm/build.sh`.

Open `index.html` here in a browser (it works from a file path, like `simulator/index.html`).

## Rebuild

```sh
pip install ziglang==0.13.0
sh simulator/hmi-preview/build.sh --test
```

`--test` runs `simulator/tests/run.js` on the preview build. The patch also moves the taps of two tests (Motor override and Remote control) to where those controls now are; every test passes.

To change the preview, edit a patched copy (`out/tree`, left by the build), then write the patch again from a clean copy with `diff -ru` or git.

## What the patch changes

Screens follow the template's grid: four columns at x 10, 127, 244 and 361, each 109 wide, rows at y 48, 104, 160 and 216.

| Screen | Change |
|---|---|
| Home | Cards 168 + 8 + 167, as the other halved pages (was 167 + 9 + 167). Card contents unchanged. |
| Menu | Rows Records (Trend, Events), Service (Valve position, Diagnostics), Setup (Settings, System). |
| Settings | Navigation tiles only: Choose sensor, Calibrate sensor, Valve setup, Control tuning, Review and save. |
| System | On the Menu, title System. Cryostat name, IP address; MAC address, Remote control; Firmware 343 wide with Restart beside it. Address in use dropped (static address only). |
| Valve position | Motor override moved here from Settings; Use last saved now 109 wide. State texts "Set by hand, not homed" and "Last saved, not homed" (were "Entered by hand, not homed" and "From last save, not homed", both clipped in the tile). |
| Readings and bars | Each bar is as wide as the widest reading its card can show ("1013.2" or "100.00") and sits right under it, right-aligned with it: 40 px on Home, 26 px on the Valve page (Ryan, 4 Nov). |
| Diagnostics | Caption column 109, tiles in columns 2 to 4. Captions Nominal, mB (was Before cal, mB) and Board, °C (was Temperature, °C) to fit 85 px. A long Last move result drops to 12 px. |
| Help | Text card 343 x 160, Up and Down 109 x 76 in the right column. |
| Up and Down | Drawn 21 x 12 chevrons (3 px stroke) on Trend, Events and Help. |
| Keypad | Last column 63 wide, so the keys end at x 353. |
| Padding | 12 px inside read-only tiles, settings fields, keypad cards, valve reading cards, fault detail, Events, Remote control, Help and the Trend legend and time labels. |
| Status strip | No full stop after a single phrase; "All settings saved 24/10/26 14:02" without the comma. |

Also in the patch, until PR #39 merges: the Set as 0 % fix from PR #39 (0.14.14, Set as 0 % reads 0.00 %), with its test change. Once #39 is on main, write the patch again without it.

Not in the patch: the Home card contents (options A, B and C are still open), event texts in DD/MM/YY (H8), and the Events list height.
