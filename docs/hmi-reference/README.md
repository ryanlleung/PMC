# PMC HMI visual-review reference — NOT an implementation specification

> **REFERENCE MATERIAL ONLY.** This folder collects ChatGPT-generated layouts and UI/UX ideas for discussion with the firmware maintainer/other agent. The PNGs were **not captured from the physical Mikromedia 4 or a running LVGL framebuffer**, and they are **not verified for exact text, touch geometry, pixel fitting, current functionality, safety or suitability for implementation**. No design is approved by the presence of these files.

## Which firmware?

- The **19 design proposals** and **27 source-based reconstructions** were created while reviewing **PMC firmware v0.10.11**, PR #14, commit `e5435e8`.
- This reference package is attached for review on **PR #15**, branch `claude/project-thread-w676ed`, which was at **v0.11.32, commit `468053c`** when the package was prepared (23 October 2026). **The latest branch has changed materially since v0.10.11.**
- Current PR #15 has real AUTO control and a rebuilt, standard-frame Motor override screen. The *old mockups incorrectly show AUTO as unavailable and propose a split motor-override workflow*; neither should be adopted without revisiting the present implementation. The designs deliberately explore alternate styling, including darker UI, and are not evidence of existing firmware UI.

## Contents

1. [Proposed redesign interactive gallery](v01011-proposal/gallery.html) — **19** visual proposals. PNGs: [screens/](v01011-proposal/screens/).
2. [Source-based reconstruction gallery](v01011-source-reconstructions/gallery.html) — **27** estimated v0.10.11 states for comparison, **not actual LVGL screen captures**. PNGs: [screens/](v01011-source-reconstructions/screens/).

Both HTML galleries embed their image data, so they can be downloaded and viewed offline. Individual PNGs are separately committed to support discussion and diffs.

## Handover to the other firmware/UI agent

**Please review; do not automatically implement or replace the existing HMI.** Start by checking the latest branch head and rebuilding/running `tools/ui-preview/build.sh` against that head. Compare *actual native LVGL screenshots at 480×272* and code interactions with each proposal. Evaluate each suggestion independently and post an assessment/plan first; request owner approval before changing code.

Evaluate specifically:

- **Operator Home:** prioritize pressure and valve readings (including `-999.9` and `2000.0` pressure, `100.00` valve), target/setpoint controls, AUTO/MANUAL/PAUSED modes, fault hierarchy, number alignment, sparkline availability, margins and touch affordance. Make it unmistakable which boxes are display-only and which are operable.
- **Valve/homing:** distinguish actions (hold-to-jog, incremental move, absolute Go to, full travel) from displayed values; show clear preconditions and fault/limit feedback. Verify operator zero and stored-position acknowledgement. Safeguards must come from actual firmware and hardware, not mockup decoration.
- **Commissioning:** review the narrow right action column, Save/Discard wording and confirmation, difference between *draft*, *in-use unsaved*, and *stored to flash*; prioritize sensor calibration, Valve settings tabs, Control tuning, Review and save and unusual states (invalid entry, unsaved values, flash failure).
- **Motor override:** inspect the **latest v0.11.32 motor_screen.c**. The proposals depicting a two-page motor layout are *exploratory only* and may be obsolete. Maintain visibility of STOP, limit switches, fault status and override state; examine current 28-pixel motor rows for touch usability without losing supported functions.
- **Trend/faults/diagnostics:** review graph axis labels, negative/invalid data, short excursions, long event text, event timestamps crossing midnight, Reset affordance, Help overlay and smaller selectors.
- **Typography and geometry:** verify every label with actual bundled fonts, 12/16/26/40 px glyphs, tabular numeric rendering, long values and standard active/disabled states. Assess both legibility and finger touch targets on the real 480×272 device.
- **State-driven testing:** capture normal/manual/moving/not-homed, AUTO/in-band/paused, pressure fault, limit hit, hardware disconnection, keypad open during fault, homing in progress/aborted, save failure, override armed/active/cancelled, and navigation while moving. Test animation/timing, clipping/overlap, heap and SRAM, and real touch release behavior.

### Design ideas worth discussing (not directives)

1. Make **display-only readings** and **editable/actionable controls** visually unambiguous with consistent treatment across Home and settings, without implying colors are user input validation.
2. Reduce the cramped effect of **two-line Save/Discard** actions and distinguish irreversible/unsafe overrides from benign navigation.
3. Keep the **operator Home's strong hierarchy** rather than redesigning everything. Numeric displays should remain right-aligned/tabular, fit the complete supported range, and never jitter horizontally.
4. Improve **unsaved-state visibility** and fault-message priority, and make status/action results truthful about controller state.
5. Standardize spacing and touch-target dimensions as far as realistically possible on a 480×272 panel, particularly on the service pages; avoid removing actual functionality simply to fit a picture.

### Safety and version caveats

- This is a design discussion, not a code review sign-off, an electrical/motion safety validation, or approval to change emergency-stop/limit logic.
- Earlier source-level issues must be **rechecked at the newest head**; fixes may already have landed.
- Several stylized concept images in the conversation included **invented controls** (temperature and auxiliary-sensor calibration, derivative tuning, two limit switches, etc.). **Do not implement unsupported features from those images.**
- Do not use the words/text/layout in the mockups as test oracle for current firmware. Source code, actual screenshots, controller behavior, and owner requirements take precedence.
- Changes, if later approved, must remain a **separate reviewable firmware commit/PR**. This package adds **reference documents/screenshots only**.

See [PR #15](https://github.com/ryanlleung/PMC/pull/15) for ongoing firmware development.
