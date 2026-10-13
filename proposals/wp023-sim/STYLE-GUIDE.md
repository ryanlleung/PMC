# PMC HMI style guide

Rev A, 13 Oct 2026. Proposed, not approved. It implements `HMI-PHILOSOPHY.md` and follows Rockwell PROCES-WP023 except where section 9 records a deviation. Pixel sizes are panel pixels at about 0.198 mm/px (480 px over a 95 mm active area, to be confirmed).

## 1. Grid

- Header 0 to 40. Four columns: 10-119, 127-236, 244-353, 361-470. Four rows: 48-96, 104-152, 160-208, 216-264. Gaps are 8 px.
- Bottom row on every screen: status strip (10-353) and STOP (361-470).
- Command buttons that move the valve directly are 12 px apart (about 2.4 mm).

## 2. Display hierarchy (WP023 s3)

| Level | Screens |
|---|---|
| 1 and 2 | Home |
| 3 | Valve, Reference valve, Trend, Alarms and events |
| 4 | Fault detail, System information, Help panels, Commissioning pages |

## 3. Colour (WP023 s8.1)

| Role | Colour |
|---|---|
| Background | `#DCE3EA` |
| Cards | `#FFFFFF` |
| Text, readings | `#1F2933` |
| Labels, units | `#546374` |
| Primary action (one per screen at most) | fill `#2D5877`, white text |
| Editable field | tint `#E9F0F6`, 3 px `#2D5877` underline, chevron |
| Field not editable | `#EEF2F6`, text `#969EA6` |
| Mode dot in header | `#C3CDD7`: filled = AUTO, ring = any other mode; `#D63031` with a fault |
| Status: info | white, grey dot |
| Status: prompt (normal, needs action) | `#E0E0E0`, black text, no icon |
| Status: warning (abnormal, no response needed) | `#3F3F3F`, white text, white warning icon |
| Status: fault | `#000000`, white text, 3 px `#D63031` border and red warning icon |
| Unacknowledged fault | the red borders blink red/`#7B8794`, 1 Hz; text never blinks |
| Unapplied commissioning change | `#6A4C93` (while no low-priority alarm class exists) |
| Trend pens | pressure `#2D5877` 2 px; target `#3F3F3F` dashed; valve `#969EA6` 1.5 px; AUTO spans `#5B6B7B` at 10 % |
| STOP | `#D63031` (deviation, section 9) |

Green and amber are not used.

## 4. Type (WP023 s8.9)

Montserrat Medium, ASCII plus degree and bullet. Sizes 12 (labels, units, status), 16 (titles, buttons, values on settings pages), 26 (home readings and setpoints), 40 (reserved for the pressure reading if the viewing distance needs it).

## 5. Numbers (WP023 s8.10, s8.12)

- Right-aligned, tabular figures.
- Pressure: 0.1 mbar on operator screens, 0.01 mbar on Sensor, Diagnostics and serial. Valve: 0.1 %.
- Units go in the tile caption (right-aligned) or in the field label.
- Lost readings show `----` in red.

## 6. Components

| Component | Look | Use |
|---|---|---|
| Secondary action | white, raised, label centred | commands |
| Primary action | filled blue | the next expected command |
| Navigation | white, raised, label left, grey chevron | opens a screen |
| Editable field | tinted, blue underline, chevron | tap opens keypad |
| Read-only value | flat, no edge | readings on detail pages |
| Status strip | card by kind (section 3) | bottom row on every screen |
| Sparkline | 143 × 32, one pen, no axes, 10 min | home tiles; tap opens Trend |
| Bar | 8 px, value marker 3 × 14 px | pressure log 0.1-1000 with low-alarm zone; valve 0-100 % |

Each command button label names the action, not the state. A second 12 px line holds only state ("3 faults", "2 of 3").

## 7. Alarms (WP023 s9)

- Classes: F codes are alarms (one priority); W codes are warnings; not referenced is a prompt; mode changes and setpoints are events (logged, not annunciated).
- A new F code is unacknowledged: blinking borders on the status strip, the header rule and the owning tile, plus buzzer bursts (3 × 2.4 kHz every 10 s).
- A tap on the status strip acknowledges all faults. Reset fault clears latches whose cause has gone, and also acknowledges.
- The fault detail page gives what happened, the controller's action, what to check, and when it can be reset.

## 8. Interaction

- Mode changes, reference, Save, Discard and Exit ask first. The confirm buttons never sit under the button that opened them.
- Keypads show the unit, the allowed range and the present value, and refuse values outside the range.
- Unavailable controls are greyed, not hidden (exception: section 9).
- Buttons act on release (firmware).
- Values refresh at 2 Hz. Touch feedback appears within 0.5 s.

## 9. Recorded deviations from WP023

| Deviation | WP023 | Reason |
|---|---|---|
| STOP is red on every screen | s8.1 p18 reserves alarm red | Operators expect a red stop. Faults are marked by a black field, a blinking border and an icon, so they do not rely on red alone. |
| One mode button changes meaning (Start AUTO, Switch to MANUAL, Resume AUTO, Reference valve) | s8.15 p38, s8.15.2 p39 | The home screen has no spare slot. Every mode change asks first, and the dialog buttons do not overlap the mode button, so a double tap cannot confirm. |
| No Home button in the header; Back goes up one level | s4.1.1 p12 | Operator pages are at most two levels deep. STOP and the status strip are on every screen. |
| Before referencing, Reference valve replaces Start AUTO | s8.3 p21 | Referencing is the required next step; the status strip says why. |
| Units in the caption rather than after the value | s8.10 p25 | Space; it is consistent across both tiles. |
| Blue for primary actions rather than for live data | s8.1 p20 | No live data is blue in the PMC, so the role is unambiguous. |
