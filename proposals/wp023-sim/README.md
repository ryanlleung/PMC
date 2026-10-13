# PMC screen simulator, WP023 proposal copy

This folder is a copy of `simulator/` at v53 (`0ed0b04`) with the changes from the review against Rockwell PROCES-WP023 (13 Oct 2026). `CHANGES.md` lists every change (S1 to S15) with its finding and acceptance check. `HMI-PHILOSOPHY.md`, `STYLE-GUIDE.md` and `OPERATOR-TEST.md` are the proposed documents. Nothing here is approved, and `simulator/` is unchanged.

Live view: https://raw.githack.com/ryanlleung/PMC/claude/project-thread-we1c6y/proposals/wp023-sim/index.html

The text below is the v53 README, unchanged.

---


Open `index.html` directly in a browser. No build or server is required. Google Fonts is optional; system fonts are used when unavailable.

Live view of the working branch: https://raw.githack.com/ryanlleung/PMC/claude/project-thread-8yhtvn/simulator/index.html

The simulator models the proposed 480 x 272 controller interface, the motor, pressure control and the cryostat flow line. It does not communicate with hardware and is not the firmware implementation. The bench controls, laptop configuration tool, serial console and physics model below the screen are simulation tools.

## UI/UX review, 9 October 2026 (v46 to v52)

- v46, operator screens and navigation: Target marked inactive or paused outside AUTO; valve card "Valve position" with Opening / Closing / Stationary and Requested; AUTO status Approaching target / Within target band / Stable; Home renamed Reference valve; Valve page without presets or the 100 % travel setting; task menu (Trend, Alarms and events, Reference valve, System information, Commissioning); Back returns one level.
- v47, commissioning access and configuration: one commissioning session (PIN 2468 in the simulator, 10 min idle timeout, shared with serial `ACCESS`); Sensor, Valve setup, Control tuning, Diagnostics and Configuration pages; every setting goes draft, Apply, Save for restart; configuration name and revision; laptop import as a draft; Target max and low alarm separate from sensor range and Target min; readings go through zero, span, cal factor or an imported table.
- v48, STOP and faults: STOPPED / FAULT in the header; STOP says the valve was not commanded closed; Reset fault replaces ACK and never starts AUTO; fault detail with what happened, controller action, what to check and reset condition. Simulator fixes: reboot loads the saved configuration and starts not referenced; device trend without the true mechanism series; event list keeps updating with 200 events stored.
- v49, follow-up review: limit changes that exclude the target move it after one confirmation; Configuration shows Not applied and Not saved lists with labels and units; calibration identity and saved state on Sensor and System information.
- v50, review of 526edf9: STOP between referencing stages ends referencing (no further move); no move starts while any fault is latched, and STOP always stops even with F-12 already latched; a revised calibration for a known sensor serial is imported as a draft and listed per serial; Apply, import and serial settings check the resulting configuration (ranges, Target min below Target max, low alarm, sensor range, start speed, profiles); a failed flash write is reported and nothing is marked saved (bench toggle "Flash write fails").
- v51, review of f2f1c1d (UI/UX, native 480 x 272):
  - Commissioning changes: drafts stay while moving between pages (no Apply/Discard question on Back); one change set, reviewed on Configuration with Unapplied changes and Applied, not saved shown separately; the same side column on every settings page (Review, Apply changes, Save all settings), Apply never turns into Save; Save asks first and says it writes the whole configuration; Discard and Exit ask first; the idle timeout closes access only, keeps drafts for the next entry and does not touch mode or valve; Apply is all or nothing.
  - Navigation: Pressure limits has its own page and tile; Valve setup groups are a labelled Valve / Reference / Drive choice; Motor direction Normal / Reversed and Pressure response Opening raises / lowers pressure (no +1 / -1 on screen); optional Setup guide (Sensor, Valve setup, Reference, Pressure limits, Tuning, Review and save); Sensor page says profiles and tables come from the PC and where an import is reviewed.
  - Operator: Switch to MANUAL on the Valve page in AUTO and paused; paused home shows Resume AUTO and Switch to MANUAL; Regulating (not Approaching) outside the band; Valve position / Requested position / Target position wording, estimated-from-steps note once; Reference valve "Find closed position"; page load labelled as a demo starting condition, Reboot board (real start) for an unreferenced boot; 1:1 size button.
  - Text fit: measured fit (16 px, 12 px, two lines, then ... with a detail view) instead of character counts; calibration identity, date and saved state in separate cards; keypad errors use both lines; keypad side panel shows In use and Saved; dialogs sized to their wrapped text; header title narrowed so NOT REFERENCED has a margin; long lists scroll by drag with a hint.
  - Icons: Back, backspace, chevrons and warning drawn as LVGL symbols (LV_SYMBOL_LEFT, RIGHT, BACKSPACE, WARNING); lock is a planned image asset. All device text is ASCII plus degree and bullet (the built-in Montserrat range).
- v52, visual pass on 14903e7 (no new features):
  - Header: one arrangement on every page, Back | title | mode, one baseline, normal colour. No countdown and no half-row header. A small static open-lock mark (planned image asset) shows while commissioning is open. Entry says once that access locks after 10 min without a touch; the idle lock shows "Commissioning locked".
  - Labels: the complete action is the main label (Apply changes, Save settings, Import from PC, Set target, Start AUTO, Switch to MANUAL, Reference valve, Enter position, Reset fault, Start reference, Discard changes, Restore saved, Load defaults). A 12 px second line only for state (Reset fault / 3 faults, Next fault / 1 of 3, Referencing / 2 of 3, setup guide step state).
  - Hints: no permanent subtitles, instruction cards, tutorial footers or colour explanations. Settings pages, Reference valve and Valve have a Help button (slot 1 of the side column, or beside the status on Valve) that opens the longer text; settings Help is built from each field's description. The status strip beside STOP stays on every screen (Ryan): faults, change counts, link and firmware, otherwise the controller status line as on home. Home button row: mode button far left, Menu above STOP (Ryan; superseded in v53).
  - Layout: operator Menu is five navigation buttons on two columns; Commissioning is six equal task buttons with Setup guide and Exit in the side column; every settings page has Help, Apply changes, Save settings in the same slots (Configuration: Discard changes in slot 1); the Setup guide uses the same grid and page names as Commissioning. The Sensor page lost the Range card (Sensor in use names it); System information drops Targets (the Set target keypad shows the allowed range).
  - Components: secondary action (white, raised, centred), primary action (filled blue), navigation (white, raised, label left, grey chevron), editable field (white, outlined, grey chevron), read-only value (flat, no edge), status strip, help panel. Values are one 16 px line, cut with "..." and opened in full on a tap; no switching between 16 px, 12 px and two lines.
  - Colour roles: blue for actions only; amber for warnings only; violet for unapplied changes; grey for movement, the valve bar and applied-but-not-saved; green good; red fault.

## Home screen from layout options, 10-11 October 2026 (v53)

Chosen by Ryan from the option renders (`previews/layout-options/`, final `17-home-final.png`):
- Two tiles, Pressure and Valve, each with the unit right-aligned in the caption, the reading right-aligned at 26 px, a bar and a setpoint box. Actions stacked above STOP: mode button, Valve, Menu.
- Pressure bar: log scale 0.1 to 1000 mbar with lines at each decade, labels 0.1, 10, 1000 centred on their lines, and a margin at each end of half the width of "1000". Valve bar: 0 to 100 % with lines at 0, 50 and 100. Both show the low-high band over the last 30 s (lighter grey where it lies over the valve fill) and a direction arrow; the pressure bar marks the target in AUTO.
- Setpoint boxes: Target (mbar) and Go to (%). Only the box for the present mode is editable (tinted, blue underline, chevron); the other is flat grey and reads "auto" or the last target.
- No default target. The first Start AUTO opens the keypad and starts AUTO on Set; after that Start AUTO confirms the remembered target or offers New target. Serial and toAuto refuse AUTO with no target set.
- Nothing is called flow: the pressure is the operator's chosen value for that cryostat.

## Tests

`tests/` drives the page with Playwright at 1:1 and writes native 480 x 272 screenshots to `tests/shots/`:

    node tests/run.js tests/workflows.js    # the seven review workflows
    node tests/run.js tests/regression.js   # earlier findings that must stay fixed, fixed header
    node tests/run.js tests/audit.js        # every screen, keypad, dialog and fault state

The audit flags clipped or cut text, text overlapping text or buttons, text outside its card, glyphs outside the LVGL font range, touch targets under 40 px and font sizes other than 12, 16, 26 and 40. It runs in Chromium with the Google Montserrat font: it does not prove rendering on the Mikromedia (LVGL glyph metrics, anti-aliasing, touch accuracy and readability on the panel still need checking there).

Page load starts referenced at 0 % in MANUAL as a demo starting condition (labelled under the screen); Reboot board (real start) boots not referenced, as the firmware will. Settings saved in the simulator are kept in the browser's local storage; the laptop panel has Erase simulated flash.

This folder is independent of `InitialTesting/` and `tools/ui-preview/` and is not part of the firmware build. Firmware has not been changed to match.
