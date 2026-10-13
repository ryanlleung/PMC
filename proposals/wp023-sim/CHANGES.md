# Simulator changes from the WP023 review

Source: review "PMC Simulator HMI Review against Rockwell PROCES-WP023", Rev A, 13 Oct 2026 (https://claude.ai/artifact/CU1HYS4QoyB4XBoWmzBrTh). Base: simulator v53 (`0ed0b04`). Decisions D1 to D7 taken as the review recommended (Ryan, 13 Oct: "fill in any gaps yourself"). Each item is built in this proposal copy (`proposals/wp023-sim/index.html`) unless marked otherwise. The live simulator in `simulator/` is unchanged; the thread "Simulator rebuild from review" ports accepted items.

Status key: **Built** = in this copy; the acceptance check is automated in `tests/wp023.js` where marked (T). **Doc** = written as a document here. **Firmware** = a note for the LVGL build, nothing to simulate.

| ID | Change | Finding, WP023 ref | Status | Acceptance check |
|---|---|---|---|---|
| S1 | Home: a 10 min sparkline in each tile (pressure and valve, one shared time axis). The pressure scale covers every reading and target in the window, so it never clips. It is at least max(1 mbar, 10 × band) tall and centred on the target in AUTO, and switches to log when the window spans more than a decade. The target is drawn as its own stepped history (dashed, AUTO spans only) with the band as a light stripe. Valve axis 0 to 100 %. Tap opens Trend. The 30 s band and direction arrow are removed. | R4; s6.7 p16, s3.1 p8, s8.12.8 p33 | Built (T) | A 5 to 8 mbar step 400 s earlier is visible on home; a 5 to 15 step shows the approach at its real size; the ±0.1 mbar band is about 5 px tall at a steady hold |
| S2 | Home: setpoint rows follow the iTC pattern, label left ("Target", "Go to") and the editable field right (x 52 to 155, 40 px). Values up to 999.9 and 100.0 fit at 26 px. Only a target above 999.9 (possible only if Target max is raised past it) drops to 16 px. The bar is 8 px with its end labels kept. | R19, iTC reference 04; s8.15.8 p43 | Built | Audit: no clipping or overlap; field is 103 × 40 px |
| S3 | Pressure shown to 0.1 mbar on operator screens (home, Valve page, keypad side panel, confirmations, status lines). Target entry accepts one decimal, and every target, serial `CTRL SP` included, is rounded to 0.1 mbar. Target band minimum 0.1 mbar. Sensor, Diagnostics and serial readings keep 0.01 mbar. | R6, D2; s8.12 p28, s8.12.2 p29 | Built (T) | Home reads "8.0"; the keypad refuses a second decimal |
| S4 | Colour roles per review section 5. There is no colour on normal states: the header dot is light grey, filled for AUTO and a ring for the other modes, and red only with a fault. Status dots are grey. The green edges on the Valve page are removed. Prompt (not referenced) is light grey with black text, a 1 px dark grey outline and no icon (the outline keeps it distinct from the page). Warning (W-02 paused, refusals) is dark grey 63 with white text and a white icon. Fault is black with white text and a red border and icon. The event list shows W- entries in dark grey. | R7, R13, D6; s8.1 p18-20, s9.2 p49, s9.8.1 p55 | Built (T) | No green or amber anywhere in normal AUTO, MANUAL or boot |
| S5 | Alarm annunciation. A new fault is unacknowledged: the status strip border and the red rule under the header blink red/grey at 1 Hz (the text does not blink), and the buzzer gives three 2.4 kHz bursts every 10 s. A tap on the status strip acknowledges every fault (logged as "acknowledged by operator") and opens the detail as before. Reset fault also acknowledges, and still needs the cause gone. The bench has a buzzer indicator and a mute button. The browser plays sound only after the first touch on the page. | R12, D4; s9 p48, s9.2 p49, s9.4 p53, s8.2 p21 | Built (T) | Blink stops on the first tap; the latch stays until Reset fault |
| S6 | The tile that owns an active alarm gets the alarm border, blinking while unacknowledged. Pressure: F-01, F-02, F-03, F-15 or invalid reading. Valve: F-07, F-08, F-09, F-10, F-13, F-16, F-17. | R13; s9.3.1 p50 | Built (T) | F-01 frames the pressure tile in red |
| S7 | The low-alarm zone is drawn on the pressure bar from the left end to the low alarm, in grey normally and red while F-02 is latched. | R5; s6.4 p15, s8.12.5 p31 | Built | Zone width follows Pressure limits > Low alarm |
| S8 | Trend legend: "Pressure, Target - - mbar. Valve % right" (the same words as home). Pens: pressure dark blue 2 px, target dark grey dashed, valve grey 1.5 px (legend word in the pen colour). Home sparklines use the same pens. AUTO spans shaded light grey. | R15; s8.12.7 p32, s6.3 p15, s8.11.5 p27 | Built | No PV/SP words; pens readable in greyscale |
| S9 | Valve page jog row (Hold to close, -1 %, +1 %, Hold to open): 12 px gaps instead of 8 px. | R10; s6.8 p16, s8.15.1 p38 | Built (T) | Gaps measured 12 px; buttons 106 px wide |
| S10 | Target max ships at 50 mbar (was the sensor full scale, 1034). It is set per cryostat in Pressure limits. | R16; s8.15.8 p43 | Built (T) | Keypad reads "Allowed 0.2 to 50" |
| S11 | Device values refresh at 2 Hz (was 10 Hz). Touches still redraw at once. | R18; s10.1 p55 | Built (T) | Last digit no longer flickers between readings |
| S12 | Bench "Physical size" button: shows the screen at 0.748 of 1:1 (95 × 54 mm on a 96 dpi monitor), for operator tests. | R3 | Built | Screen measures about 95 mm wide on a 96 dpi monitor |
| S13 | HMI philosophy, style guide and operator test protocol. | R1, R3, D7; s1 p4, s1.1 p5, s6.1 p15, s8.11.5 p28 | Doc | `HMI-PHILOSOPHY.md`, `STYLE-GUIDE.md`, `OPERATOR-TEST.md` |
| S14 | Documented deviations, no change: red STOP (R8, D5), one mode slot with confirmation (R9, D3), Back only (R14), Reference valve shown instead of a disabled Start AUTO (R17), units in captions (R19). | s8.1, s8.15, s7, s8.3, s8.10 | Doc | Listed in `STYLE-GUIDE.md` section 9 |
| S15 | Firmware: trigger buttons on release (`LV_EVENT_CLICKED`), not on press. Draw the sparklines as `lv_chart` with 120 points (one per 5 s). Drive the buzzer from the Mikromedia piezo (confirm it is fitted on the STM32F4 board). Measure call-up time (2 s or less) and touch feedback (0.5 to 2 s) on the panel. | R10, R12, R18; s10.2, s10.3 p55 | Firmware | Measured on the mm4 |
| S16 | Menu: the Alarms and events button carries the alarm state. With a fault latched it shows a second line ("1 fault, not acknowledged") and the alarm border, blinking while unacknowledged. | Menu review M1; s9.3.1 p50 | Built (T) | F-01 shows "1 fault, not acknowledged" on the Menu |
| S17 | Menu footer shows the process status like every other page (was "Link USB. Firmware ..."). Link and firmware stay on System information. | M2; s6.7 p16, Ryan 9 Oct (status strip on every screen) | Built (T) | Menu footer reads "Stable at 5.0 mbar ..." in AUTO |
| S18 | Diagnostics moves onto the Menu and opens without the PIN. It is read only, and the fault detail pages send the operator to the readings it holds. It stays on the Commissioning page too. The Menu now fills its 2 × 3 grid. | M3; s3.1 p8 (level 4 for troubleshooting) | Built (T) | Diagnostics opens with commissioning locked |
| S19 | Reference valve page in operator terms: "Valve, %" instead of "Steps now", "Seat switch: At seat / Off seat" instead of "Limit switch: Open / Closed" (an NC contact reads backwards to an operator), and "Past switch" removed. Step counts and the contact state stay on Diagnostics. | M4; s8.12 p28, s8.9 p24 | Built (T) | No step counts on the page |
| S20 | Alarms and events: each latched fault shows its state under the time ("Not acknowledged", "Acknowledged, cause present", "Cause gone, Reset fault clears"). | M5; s9.8.1 p55 | Built (T) | F-01 after re-plugging reads "Cause gone, Reset fault clears" |
| S21 | Event history in text colour; red is kept for the active faults above it. Event words match the screen: Referencing/Referenced (was Homing/Homed, tag REF was HOME), "Auto at target 5.0 mbar" (was "at SP 5.00"), target changes "TARGET Set to 5.0 mbar" (was "SP Setpoint 5.00"). The serial EVT lines change the same way (firmware note). | M6; s9.8.1 p55, R2 terms | Built (T) | No HOME, Homed, Setpoint or "at SP" in the event log |

## Menu and lower pages, 13 Oct

Reviewed after Ryan asked (13 Oct). The first review only treated the Menu as a navigation level. S16 to S21 come from this pass. Reviewed with no change:

- Commissioning page, Setup guide, Sensor, Valve setup (three tabs), Pressure limits, Control tuning, Configuration: field, navigation and side-column patterns are consistent with `STYLE-GUIDE.md` section 6. Engineering units and 0.01 mbar are right at level 4.
- The open padlock beside Back while commissioning is open is small, but it is Ryan's v52 decision (static mark, no countdown). The operator test can check it.
- The event list keeps the full date with year and seconds (Ryan, v45).
- Diagnostics abbreviations ("Uncal.", "Switch, NC") are kept: the page is for the commissioning engineer, and the 109 px cards have no room for more. "Board, C" now reads "Board, °C".

## Reversals of Ryan's earlier decisions

These are built here because Ryan asked for the gaps to be filled (13 Oct), but each one undoes an earlier choice of his and needs his yes before porting:

| Change | Earlier decision |
|---|---|
| S1 removes the 30 s low-high band and direction arrow | 30 s band, Ryan 12 Oct |
| S1 and S2 rearrange the tile: sparkline added, bar slimmer, setpoint box becomes a label-left row | layout 17-home-final, Ryan 11 Oct (d1 field style and u2 units kept) |
| S3 pressure at 0.1 mbar on operator screens | "full pressure digits to 1034.00", Ryan 10 Oct |
| S4 header dot grey (filled AUTO, ring otherwise) instead of the mode colour | mode word + coloured dot, Ryan 9 Oct |
| S5 tap acknowledges, separate from Reset fault | "Reset fault replaces ACK", v48 |
| S2 field may drop to 16 px above 999.9 | v52 one size per value |

Open: Ryan's pick of the iTC looks (`previews/layout-options/20-home-itc-style.png`, A, B or C). This proposal uses the present light scheme. Its structure and colour roles carry over to option B (black background, white values). Option A's yellow values conflict with S4.

## Independent evaluation, 13 Oct

Full write-up: `/mnt/project-files/reviews/2026-10-13-wp023-proposal-evaluation.md`. Fixed after it: sparkline clipping and the target history (1), reversals now listed (2), prompt strip outline (3), serial target rounding and band minimum (5), buzzer resume (6), the Go to field fit, the Trend legend colour and the home/Trend pen match. Not changed, with reasons:

- Warning and fault strips differ mainly by the red border (4). This is WP023-compliant; it is added to the operator test.
- No "10 min" label on home. There is no room at 12 px; Trend gives the time axis.
- The jog row is not on the 4-column grid (x 10/128/246/364). This is the cost of the 12 px gaps.
- The sparkline is a navigation target without a chevron. It is listed in the style guide; the operator test checks whether anyone finds it.
- There is no corner icon on the tile alarm border. The status strip carries the icon; the tile has no room.
- Physical size assumes a 96 dpi monitor. The operator test says to check with a ruler.

## Not changed, open

- R11 reading size: 26 px is kept until the panel's distance from the temperature controller is known. If it is over about 0.8 m, use 40 px for the pressure reading (`OPERATOR-TEST.md` records the distance).
- R2, R3: the operator walkthrough has not been run. `OPERATOR-TEST.md` is the script.

## Test run

`node tests/run.js tests/wp023.js` (the S-item checks), `tests/workflows.js`, `tests/regression.js` and `tests/audit.js` in this folder all pass, and the audit has no findings (13 Oct 2026, after the evaluation fixes and the Menu pass). The audit checks text fit, overlap and target size only; usability needs `OPERATOR-TEST.md`.
