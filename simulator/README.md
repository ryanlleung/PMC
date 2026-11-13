# PMC simulator

Open `index.html` in a browser (from a file or a web server; no build needed to use it).

From 0.11.38 the simulator runs the mm4 firmware itself. `pmc-wasm.js` is the firmware in `InitialTesting/` (screens, valve and homing, AUTO, settings store, calibration, faults, event log, trend and the text port's command set) compiled to WebAssembly, so the screens, messages, fault numbers, settings and behaviour are the board's, not a copy of them. Only the hardware underneath is simulated:

| On the board | In the simulator |
|---|---|
| Display and touch panel | 480 x 272 canvas, mouse or finger (a press lasts at least 80 ms) |
| Stepper 3 outputs, TIM7 step interrupt | `stepper3.c` unchanged, its timer run per simulated microsecond count; the four outputs turn a simulated motor, so the direction and coil order selected on Settings > Motor override > Coils act as on the bench |
| Needle valve, limit switch | flow-line needle, 16512 steps a turn, seats 30 steps past the switch; switch faults: broken wire, stuck, trips early |
| Druck through the INA228 | the cryostat flow line read through the 5 % input loading; open to room air for calibration. Two flow lines (Cryostat > Flow line): the bench cryostat (default), fitted to the bench logs of 6 and 9 Nov 2026, and the physics model (v63, as `tools/ctrl-test/model.c`) |
| Boost 10, motor 12 V supply (the motor stalls without it; the firmware does not sense it from 0.14.3), INA228 die temperature | bench controls (excitation, supply volts, controller temperature) |
| Serial flash (settings, saved position) | 8 MB in memory, kept in the browser between visits |
| Port 8888 | the console under the screen (the page shows each line sent; the board does not echo) |
| RTC | the computer's clock at power-up |

The bench cryostat is the bench as its logs show it (valve, pressure and the Lake Shore's heat exchanger temperature): the pressure heads for a value set by the needle's opening and how cold the heat exchanger is, after a 2 s dead time, through a lag of 25 to 70 s while the heat exchanger holds liquid (under about 3.5 K) and about 5 s when it is dry; below about 6 mbar it holds none, so the lag there is 2 to 8 s whatever the temperature reads. The heat exchanger follows the pressure, colder from 1 to 12 mbar and warmer again above: up to 4.5 K within seconds (the liquid boils off), the rest over minutes, so the pressure creeps after a move: on up at 4 to 12 mbar (the valve's knee) and back above 12 mbar. Below about 3 % open there is almost no flow. At low openings the needle sticks when opening (it slips all at once after a 0.37 % move) and follows at once when closing. Run from the logged valve, it gives each setpoint's pressure within 1 to 2 mbar and each relay test's swing and phase within about a third of the bench's. Run as a board under the same relay tests, the period comes out within about a quarter of the bench's (9 to 28 s from 4 to 24 mbar) and the swing within a factor of two, as the heat exchanger's state depends on what came before. `wasm/fit_bench.py <log.csv> ...` prints that comparison for any GUI session or autotune log; its numbers are bench.c's. So a reach from 10 down to 1 mbar with the heat exchanger cold and a high KP (1.20, 10 Nov) swings over the knee as on the bench, and with KP 0.10 comes down smoothly. Its Druck reads true with no calibration (its pressures are what the calibrated bench board read). The heat exchanger temperature shows under Cryostat. At power-on it is 3 K (the cryostat has been running).

The bench panel switches the helium supply and the rotary pump, unplugs the Druck or the Power Monitor, lowers the excitation, sets the motor supply, slips the needle, breaks or sticks the limit switch, heats the controller and turns the needle by hand. Speed runs at real time, 10 x, 60 x or as fast as the computer allows (about 1000 x with the screen still; a finger on the screen always runs at real time). Power cycle restarts the firmware with the flash kept; New board erases it.

## Building

`wasm/build.sh` rebuilds `pmc-wasm.js` from `InitialTesting/` after a firmware change. It needs zig 0.13 for its WebAssembly C library (`pip install ziglang==0.13.0`; any OS) and a shell. The firmware sources are used as they are, except sed lines for fixed hardware addresses in `stepper3.c` (NVIC, PRIMASK); the script stops if those lines change. `main.c` is compiled in whole and `sim_run()` repeats its loop body (`wasm/sim_main.c`).

The stand-ins are in `wasm/`: `include/` (register structs for GPIO, RCC, TIM7, ADC1, RTC; LVGL configuration = the firmware's heavy `lv_conf.h` without the CCM section), `sim_main.c` (registers, timer, flash, USB serial, display, touch, the page's calls), `sim_clicks.c` (`clicks.c` with readings from the bench), `bench.c` (flow line, motor, switch, Druck, supply).

## Tests

- `node simulator/tests/run.js [--png dir]`: bench scenarios against the build, in node, on the physics flow line unless a case says otherwise: boot and bundled firmware-version checks, homing from the screen, Set position, MANUAL move prerequisites, independent homing Return, motor power loss, tuning saves in AUTO, F-1 to F-6 (except F-5), W-2, W-3, settings and saved position over a power cycle, and the remote command set's reply rules and error codes (0.13.0); on the bench cryostat, AUTO holding 16 mbar with KP 0.40 TI 0.75, a relay there cycling at about the bench's period, a small opening move sticking, and a reach from 10 to 1 mbar swinging at KP 1.20 but not at KP 0.10.
- `node simulator/tests/remote-only.js`: the text port's command set: unrecognised commands answer ERR 100 with Remote control On (ERR 211 with it Off), homing on the screen, then the valve and AUTO from the link.
- `node simulator/tests/tcp-board.js [port] [speed] [--remote-off] [--physics]`: serves the simulated board on a TCP port (8888 by default), homed and in MANUAL at 5 % open, on the bench cryostat (`--physics`: the physics flow line). Homing uses the `sim_home` test hook, as the screen does; startup fails if homing or the move fails. Remote control is On unless `--remote-off` is passed. Pass port 0 to request an available local port.
- `node simulator/tests/tcp-board-test.js`: starts that helper with remote writes On and Off, and checks its homed 5 % startup, query replies, setpoint writes and STOP refusals.
- `node simulator/tests/page.js [shot.png]`: the page in Chromium (Playwright): taps, homing, a console command, settings kept over a reload.

## Not simulated

- W-5 (move timed out; F-5 to 0.16.28) cannot happen by itself: the step interrupt is never late here. The tests provoke it with bench value 24 (`sim_set(24, 1)`: the step interrupt does not run), which the page has no control for.
- Ethernet / Modbus TCP (the build is the USB one, as `pmc_config.h`), the mikroBootloader, the watchdog's timing (an LVGL assert stops the page and restarts the firmware with reset reason watchdog).
- Real speed: redraw times are the browser's, not the STM32's. `PERF?` is no longer a text-port command (ERR 100 with Remote control On). The simulator has the board's 64 KB LVGL heap from 0.16.28 (64 + 24 KB, the removed USB build's, before).
- The Druck and Power Monitor are models. The bench cryostat is fitted to two days of one bench's logs: another cryostat, or this one on another day (heater power, helium level), will differ, and its noise and wander are only roughly the bench's.

## Before 0.11.38: the JavaScript simulator (v1 to v62)

The screens were designed in a JavaScript simulator (v62 on 14 Oct 2026, commit 3345851) and then ported to the firmware in stages 4 to 8. The notes below and `HMI-PHILOSOPHY.md`, `STYLE-GUIDE.md`, `WP023-CHANGES.md` and `OPERATOR-TEST.md` are from that time; where they differ from the firmware, the firmware is right. To open v62: `git show 3345851:simulator/index.html > v62.html`.

### UI/UX review, 9 October 2026 (v46 to v52)

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

### Home screen from layout options, 10-11 October 2026 (v53)

Chosen by Ryan from the option renders (`previews/layout-options/`, final `17-home-final.png`):
- Two tiles, Pressure and Valve, each with the unit right-aligned in the caption, the reading right-aligned at 26 px, a bar and a setpoint box. Actions stacked above STOP: mode button, Valve, Menu.
- Pressure bar: log scale 0.1 to 1000 mbar with lines at each decade, labels 0.1, 10, 1000 centred on their lines, and a margin at each end of half the width of "1000". Valve bar: 0 to 100 % with lines at 0, 50 and 100. Both show the low-high band over the last 30 s (lighter grey where it lies over the valve fill) and a direction arrow; the pressure bar marks the target in AUTO.
- Setpoint boxes: Target (mbar) and Go to (%). Only the box for the present mode is editable (tinted, blue underline, chevron); the other is flat grey and reads "auto" or the last target.
- No default target. The first Start AUTO opens the keypad and starts AUTO on Set; after that Start AUTO confirms the remembered target or offers New target. Serial and toAuto refuse AUTO with no target set.
- Nothing is called flow: the pressure is the operator's chosen value for that cryostat.

### Ryan's batch, 13 October 2026 (v54)
- Home: pressure shown to 1 dp everywhere on screen; worst cases (1034.9 mB, target 1034.0, 100.0 %, two-line status) checked by the audit in MANUAL and AUTO. The pressure bar is filled like the valve bar. The Target box says "Not set" before the first target (taken from the Mercury iTC).
- Units and symbols: mB for mbar abs on screen, @ for "at" in status and confirms (Stable @ 5.0 mB ±0.1 for 60 s), ± for plus or minus and the keypad sign key. The serial console keeps mbar. **± (U+00B1) is not in LVGL's built-in Montserrat fonts: the firmware font must be converted with U+00B1 added** (the audit allows it).
- Valve page: Fully close (0 %) and Fully open (100 %) replace the what-the-valve-is-doing card (it repeated the status strip); each asks first.
- Diagnostics: three rows named at the left (Pressure sensor, Valve motor, Last move); Closed switch reads Pressed / Not pressed; board temperature and link in the footer.
- Names in operator terms: Reference valve is Zero valve (Zeroing, Zeroed, NOT ZEROED); Trend is Pressure trend; Alarms and events is Faults and log; System information is About this unit; Commissioning is Engineer settings (Engineer PIN, Lock settings); Configuration is Review and save; Sensor is Pressure sensor.
- A message in the status strip has an OK button that dismisses it.
- Stage then Apply (liked in the iTC screens) was already how Engineer settings work: changes are drafts until Apply changes.

### HMI style guide review ported, 13 October 2026 (v55)
- All WP023 review changes S1 to S23 from the proposal copy (thread "Simulator review against HMI style guide", 9f7372e) are in, listed in `WP023-CHANGES.md`. HMI philosophy, style guide and operator test are `HMI-PHILOSOPHY.md`, `STYLE-GUIDE.md`, `OPERATOR-TEST.md`; the S-item checks are `tests/wp023.js`.
- Reversals of earlier decisions taken with it (Ryan to veto): 30 s band and arrow replaced by a 10 min sparkline; home tile rearranged (sparkline, slimmer bar, label-left setpoint row); header dot grey (filled AUTO, ring otherwise); tap acknowledges a fault, separate from Reset fault; a target above 999.9 drops to 16 px. Pressure at 0.1 mbar was already v54.
- Home Target and Go to boxes (Ryan, 13 Oct): everything in them is 16 px, the size of "Not set", and the box is 82 px (a little over "Not set" plus the chevron); 1034.0 fits. The OK on status messages is one narrow 60 px button everywhere.
- Where the review and v54 clash, v54 holds (names, mB/@/±, Diagnostics layout, Fully close/open, filled pressure bar, Not set, OK button).

### Live calibration, 13 October 2026 (v56)
- Engineer settings has a Live calibration page in line with firmware 0.3.30 (`CAL VAC`, `CAL AIR`): Capture vacuum (pumped down, 0 mB unless typed, ±0.01) and Capture air (vented, room barometer reading 500 to 1100 mB, ±1.0) each average 10 live readings through the board. Either point can be redone alone. Once both are held the two-point table is an unapplied change; the page shows the signal, the reading now and the reading with the new calibration, then Apply changes and Save settings as on every settings page.
- Certificate zero, span and the cal factor are removed (inaccurate: the INA228 loads the Druck output). Pressure sensor keeps Sensor in use, the calibration detail (kind, points, state) and Import from PC. About this unit shows Cal type instead of Cal factor. Saved config key is now v6, so older saved settings are dropped.
- Duplications removed from Engineer settings: the Diagnostics tile (Diagnostics is on the Menu without the PIN) and the Setup guide (it repeated the six tiles). Each tile now shows where it stands; Review and save shows no count because the status strip carries it.

### Ryan's batch, 14 October 2026 (v57)
- Pressure limits removed: the target is 0 to the sensor range, F-2 only above the range, no low alarm. Target band moved to Control tuning.
- A reading a little below 0 (calibration drift) is shown as it is (for example -0.4), with no fault; the target can be 0.
- No engineer mode: no PIN, no Lock, no idle lock, no ACCESS commands. Menu > Settings has four tiles (Pressure sensor, Valve setup, Control tuning, Review and save).
- Review and save keeps the previous saved revision. v60: Restore saved, Load defaults and Revert are one Load settings button listing Last save, Previous save and Factory defaults; any of them comes in as unapplied changes (Apply, then Save). Saved config key is now v7.
- Date and time on the home header (removed again the same day, Ryan 14 Oct).
- Console: HELP lists one command per line under its group; ? queries give one value or related group per line; a refused import lists one reason per line.
- Pressure sensor page: Sensor in use and Add sensor; Calibration and Remove sensor; Live calibration and Import from PC. Sensors are named by range in psi (15 psi, s/n 5880156); Add sensor asks for the serial, then the range (5 to 100 psi), and starts uncalibrated.
- Live calibration opens from Pressure sensor. Capture vacuum asks nothing (0 mB; Not 0 mB if a gauge reads more). Capture air asks for the barometer reading and shows what the sensor reads now beside the keypad.

### Limit question and time range, 14 October 2026 (v58)
- Home tiles say "10 min" beside the reading: the time range of the sparkline.
- The closed switch is force-actuated and can trip before the needle seats (ice, mechanical fault). Every hit (v59) asks "Set this point as 0 %?" with Skip (count kept) or Set as 0 %. Closing stays blocked while the switch is pressed.
- Settings > Limit override (v59) lets the valve close past a pressed switch, for a stuck switch. It asks first and covers the next move only, ending when that move stops (as firmware MOT OVERRIDE, 0.3.32); while armed the status strip says so on every page.

### Trend, 14 October 2026 (v60)
- Pressure trend renamed Trend (it shows the valve too). The range is up and down buttons either side of a Range card, cycling 1, 2, 5, 10, 30 min and 1 hour; one hour of samples is kept.
- Diagnostics "Past switch" renamed "Beyond switch": closing steps taken after the valve passed the closed-switch point (the switch did not stop it, or Limit override). Should stay 0.

### One-step save, 14 October 2026 (v61)
- Ryan chose one step (decision card): Apply changes is gone. Edits are violet until Save settings, which checks them together (all or nothing), puts them in use and writes the whole configuration to flash as the next revision. The revision before stays as Load settings > Previous save, the undo.
- Settings pages: Help, Save settings, Discard changes. Review and save: Unsaved changes (one list), Last saved, Configuration, Load settings. The strip says "N unsaved changes. Save settings to use."
- Changes that take effect at once (serial PID/VALVE commands, Add/Remove sensor) count as unsaved changes until Save settings.

### Menu tidy, 14 October 2026 (v62)
- Menu: Trend, Faults and log, Calibrate valve, Calibrate sensor, Diagnostics, Settings. About this unit removed (firmware version moved to the Diagnostics strip); Live calibration renamed Calibrate sensor and opened from the Menu.
- Zero valve renamed Calibrate valve (closes to the seat switch to find 0 %); NOT ZEROED / ZEROING / Zeroed read NOT CALIBRATED / CALIBRATING / Calibrated. Valve setup tab Zero renamed Calibrate.
- Pressure sensor page: Import from PC removed.

### Tests (to v62)

The v62 tests (`workflows.js`, `regression.js`, `audit.js`, `wp023.js`) drove that page with Playwright and are in git with it; they were removed in 0.11.38 with the page.

From 0.13.4 the simulator has the board's Remote control switch (Settings > System). From 0.13.7 it is On at power-up, as on the board; turned Off on the screen, the page's console answers set commands with ERR 211. `boot(flash, { remote: false })` in the node tests turns it Off.

From 0.13.7 the simulator takes a second finger, for the two-finger Limit override (Settings > Motor override): on a touchscreen, put a second finger down; with a mouse, Shift+click holds a press down until the next Shift+click, and a plain click is then the second finger. The node tests use `sim_touch2(x, y, down)`.
