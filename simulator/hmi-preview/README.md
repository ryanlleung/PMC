# HMI template preview

The simulator with the common HMI template changes applied, for a look before any firmware change. The base is the firmware on this branch (0.15.8, from the w676ed branch). The firmware in `InitialTesting` is not changed: `build.sh` copies it, applies `hmi-template.patch` to the copy and compiles that with `simulator/wasm/build.sh`.

Open `index.html` here in a browser (it works from a file path, like `simulator/index.html`).

## Rebuild

```sh
pip install ziglang==0.13.0
sh simulator/hmi-preview/build.sh --test
```

`--test` runs `simulator/tests/run.js` on the preview build. The patch also moves the taps of the tests whose controls moved; every test passes.

To change the preview, edit a patched copy (`out/tree`, left by the build), then write the patch again from a clean copy with `diff -ru` or git.

## What the patch changes

Screens follow the template's grid: four columns at x 10, 127, 244 and 361, each 109 wide, rows at y 48, 104, 160 and 216.

| Screen | Change |
|---|---|
| Header | The same on every screen (Ryan, 4 Nov): two icons 44 wide at the left, then the title from x 96, left-aligned. The first slot is Home on every page and Power on Home (Ryan: Home is the more useful); Back beside it, hidden on Home except to close a popup. Home and Power are greyed under a popup. Home's title falls back to the cryostat name alone if "<name> pressure controller" does not fit. The mode panel starts at x 357, the middle of the gap between the columns below (was 351). |
| Power | On Home, top left. Its question has Cancel, Restart and Shut down (Ryan: Restart in the power button as well); Restart asks its own question, as on System. Three-button questions are 376 wide so "Shut down" fits on one line. |
| Home | Cards 168 + 8 + 167, as the other halved pages (was 167 + 9 + 167). Start AUTO, Stop AUTO and Home valve sit under the bar, as wide as it. Start AUTO filled dark blue with white lettering (Ryan, 4 Nov; was outlined). Trend back in the right column in place of the IP tile (Ryan, 4 Nov); the address is on System. |
| Menu | Rows Records (Trend, Events), Service (Valve position, Diagnostics), Setup (Settings, System). |
| Settings | Navigation tiles only: Choose sensor, Calibrate sensor, Valve setup, PID tuning, Review and save. |
| System | On the Menu, title System. Cryostat name, IP address; MAC address, Remote control; Firmware 343 wide with Restart beside it. Address in use dropped (static address only). |
| Valve position | Motor override moved here from Settings; Use last saved now 109 wide. |
| Readings and bars | Each Home bar is as wide as the widest reading its card can show ("1013.2" or "100.00") and sits right under it, right-aligned with it (Ryan, 4 Nov). |
| Diagnostics | Caption column 109, tiles in columns 2 to 4. Captions Nominal, mB (was Before cal, mB) and Board, °C (was Temperature, °C) to fit 85 px. A long Last move result drops to 12 px. |
| Help | Text card 343 x 160, Up and Down 109 x 76 in the right column. |
| Up and Down | Drawn 21 x 12 chevrons (3 px stroke) on Trend, Events and Help. |
| Keypad | A popup over the dimmed page (Ryan, 4 Nov: an operator took the large present value for the entry): box 343 x 216 on columns 1 to 3, STOP beside it. Title, then the present value ("Valve now 0.00 %") and any instruction line in small text on the left, centred together on the entry field's middle; the entry in a blue-edged field on the right. No range shown: Set on an entry out of range says "Out of range: 0.00 to 100.00" in place of those lines (Ryan, 4 Nov). Obvious hint lines dropped ("0 closed, 100 open", "On the Druck label", steps per turn, the Allowed and Range lines); the two instructions on the calibration keypads stay. Keys 56 x 44, Cancel over Set. An entry too long for 26 px (8-figure serial numbers) drops to 16 px. |
| Padding | 12 px inside read-only tiles, settings fields, keypad cards, valve reading cards, fault detail, Events, Remote control, Help and the Trend legend and time labels. |
| Status strip | No full stop after a single phrase; "All settings saved 24/10/26 14:02" without the comma. |

Not in the patch: the Home card contents (options A, B and C are still open), event texts in DD/MM/YY (H8), and the Events list height.
