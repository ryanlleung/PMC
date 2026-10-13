# Simulator changes from the WP023 review

Source: review "PMC Simulator HMI Review against Rockwell PROCES-WP023", Rev A, 13 Oct 2026 (https://claude.ai/artifact/CU1HYS4QoyB4XBoWmzBrTh). Base: simulator v53 (`0ed0b04`). Decisions D1 to D7 taken as the review recommended (Ryan, 13 Oct: "fill in any gaps yourself"). Each item is built in this proposal copy (`proposals/wp023-sim/index.html`) unless marked otherwise. The live simulator in `simulator/` is unchanged; the thread "Simulator rebuild from review" ports accepted items.

Status key: **Built** = in this copy and covered by the test run below. **Doc** = written as a document here. **Firmware** = a note for the LVGL build, nothing to simulate.

| ID | Change | Finding, WP023 ref | Status | Acceptance check |
|---|---|---|---|---|
| S1 | Home: a 10 min sparkline in each tile (pressure and valve, one shared time axis). Pressure axis in AUTO is linear, centred on the target, ±max(0.5 mbar, 5 × target band), with the band as a light grey stripe and the target dashed. Without a target in use it autoscales (min span 1 mbar, log above one decade). Valve axis 0 to 100 %. Tap opens Trend. The 30 s band and direction arrow are removed. | R4; s6.7 p16, s3.1 p8, s8.12.8 p33 | Built | A 5 to 8 mbar step 400 s earlier is visible on home; the ±0.1 mbar band is about 7 px tall |
| S2 | Home: setpoint rows follow the iTC pattern, label left ("Target", "Go to") and the editable field right (x 58 to 155, 40 px). The field drops to 16 px digits if the value does not fit at 26 px. The bar is 8 px with its end labels kept. | R19, iTC reference 04; s8.15.8 p43 | Built | Audit: no clipping or overlap; field is 97 × 40 px |
| S3 | Pressure shown to 0.1 mbar on operator screens (home, Valve page, keypad side panel, confirmations, status lines). Target entry accepts one decimal. Sensor, Diagnostics and serial keep 0.01 mbar. | R6, D2; s8.12 p28, s8.12.2 p29 | Built | Home reads "8.0"; the keypad refuses a second decimal |
| S4 | Colour roles per review section 5. There is no colour on normal states: the header dot is light grey, filled for AUTO and a ring for the other modes, and red only with a fault. Status dots are grey. The green edges on the Valve page are removed. Prompt (not referenced) is light grey with black text and no icon. Warning (W-02 paused, refusals) is dark grey 63 with white text and a white icon. Fault is black with white text and a red border and icon. The event list shows W- entries in dark grey. | R7, R13, D6; s8.1 p18-20, s9.2 p49, s9.8.1 p55 | Built | No green or amber anywhere in normal AUTO, MANUAL or boot |
| S5 | Alarm annunciation. A new fault is unacknowledged: the status strip border and the red rule under the header blink red/grey at 1 Hz (the text does not blink), and the buzzer gives three 2.4 kHz bursts every 10 s. A tap on the status strip acknowledges every fault (logged as "acknowledged by operator") and opens the detail as before. Reset fault also acknowledges, and still needs the cause gone. The bench has a buzzer indicator and a mute button. | R12, D4; s9 p48, s9.2 p49, s9.4 p53, s8.2 p21 | Built | Blink stops on the first tap; the latch stays until Reset fault |
| S6 | The tile that owns an active alarm gets the alarm border, blinking while unacknowledged. Pressure: F-01, F-02, F-03, F-15 or invalid reading. Valve: F-07, F-08, F-09, F-10, F-13, F-16, F-17. | R13; s9.3.1 p50 | Built | F-01 frames the pressure tile in red |
| S7 | The low-alarm zone is drawn on the pressure bar from the left end to the low alarm, in grey normally and red while F-02 is latched. | R5; s6.4 p15, s8.12.5 p31 | Built | Zone width follows Pressure limits > Low alarm |
| S8 | Trend legend: "Pressure, Target - - mbar. Valve % right" (the same words as home). Pens: pressure dark blue 2 px, target dark grey dashed, valve grey 1.5 px. AUTO spans shaded light grey. | R15; s8.12.7 p32, s6.3 p15, s8.11.5 p27 | Built | No PV/SP words; pens readable in greyscale |
| S9 | Valve page jog row (Hold to close, -1 %, +1 %, Hold to open): 12 px gaps instead of 8 px. | R10; s6.8 p16, s8.15.1 p38 | Built | Gaps measured 12 px; buttons 106 px wide |
| S10 | Target max ships at 50 mbar (was the sensor full scale, 1034). It is set per cryostat in Pressure limits. | R16; s8.15.8 p43 | Built | Keypad reads "Allowed 0.2 to 50" |
| S11 | Device values refresh at 2 Hz (was 10 Hz). Touches still redraw at once. | R18; s10.1 p55 | Built | Last digit no longer flickers between readings |
| S12 | Bench "Physical size" button: shows the screen at 0.748 of 1:1 (95 × 54 mm on a 96 dpi monitor), for operator tests. | R3 | Built | Screen measures about 95 mm wide on a 96 dpi monitor |
| S13 | HMI philosophy, style guide and operator test protocol. | R1, R3, D7; s1 p4, s1.1 p5, s6.1 p15, s8.11.5 p28 | Doc | `HMI-PHILOSOPHY.md`, `STYLE-GUIDE.md`, `OPERATOR-TEST.md` |
| S14 | Documented deviations, no change: red STOP (R8, D5), one mode slot with confirmation (R9, D3), Back only (R14), Reference valve shown instead of a disabled Start AUTO (R17), units in captions (R19). | s8.1, s8.15, s7, s8.3, s8.10 | Doc | Listed in `STYLE-GUIDE.md` section 9 |
| S15 | Firmware: trigger buttons on release (`LV_EVENT_CLICKED`), not on press. Draw the sparklines as `lv_chart` with 120 points (one per 5 s). Drive the buzzer from the Mikromedia piezo (confirm it is fitted on the STM32F4 board). Measure call-up time (2 s or less) and touch feedback (0.5 to 2 s) on the panel. | R10, R12, R18; s10.2, s10.3 p55 | Firmware | Measured on the mm4 |

## Not changed, open

- R11 reading size: 26 px is kept until the panel's distance from the temperature controller is known. If it is over about 0.8 m, use 40 px for the pressure reading (`OPERATOR-TEST.md` records the distance).
- R2, R3: the operator walkthrough has not been run. `OPERATOR-TEST.md` is the script.

## Test run

`node tests/run.js tests/workflows.js`, `tests/regression.js`, `tests/audit.js` in this folder: all pass, audit no findings (13 Oct 2026).
