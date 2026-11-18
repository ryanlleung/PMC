# iPC simulator

Open `index.html` in a browser (from a file or a web server; no build needed to use it).

The simulator runs the firmware itself. `ipc-wasm.js` is the firmware in `firmware/` (screens, valve and homing, AUTO, settings store, calibration, faults, event log, trend and the text port's command set) compiled to WebAssembly, so the screens, messages, fault numbers, settings and behaviour are the board's, not a copy of them. It is rebuilt with every firmware version, so it matches the firmware beside it. Only the hardware underneath is simulated:

| On the board | In the simulator |
|---|---|
| Display and touch panel | 480 x 272 canvas, mouse or finger (a press lasts at least 80 ms) |
| Stepper 3 outputs, TIM7 step interrupt | `stepper3.c` unchanged, its timer run per simulated microsecond count; the four outputs turn a simulated motor, so Direction (Motor > Settings) acts as on the bench. The coil order is the saved one, 0-2-1-3 from the factory; nothing on the screen or the text port changes it |
| Needle valve, limit switch | flow-line needle, 16512 steps a turn, seats 30 steps past the switch; switch faults: broken wire, stuck, trips early |
| Druck through the Power Monitor Click | the cryostat flow line read through the 5 % input loading; open to room air for calibration. The flow line is a physics model, the same as `tools/ctrl-test/model.c`: the helium bath, the needle valve, the pot and the rotary pump |
| Boost 10, motor 12 V supply (the motor stalls without it; the firmware does not sense it), Power Monitor Click die temperature | bench controls (excitation, supply volts, controller temperature) |
| Serial flash (settings, saved position) | 8 MB in memory, kept in the browser between visits |
| Port 8888 | the console under the screen (the page shows each line sent; the board does not echo). The build is the host build (`IPC_LINK_ETHERNET 0`), so there is no Ethernet or Modbus |
| RTC | the computer's clock at power-up |
| Backlight | the screen dims to 10 % after 10 min with no touch and goes dark 30 s into OFF, as on the board |

The flow line gives 0.8 to 12.5 mbar over 20 to 100 % open, far less pressure per % open than a cryostat, so AUTO with the factory PID gains is very slow on it. KP 4.00 and TI 0.25 in every zone suit it; the page says so under Cryostat, and the tests set those gains where they test something other than the gains.

The bench panel switches the helium supply and the rotary pump, unplugs the Druck or the Power Monitor, lowers the excitation, sets the motor supply, slips the needle, breaks or sticks the limit switch, heats the controller and turns the needle by hand. Speed runs at real time, 10 x, 60 x or as fast as the computer allows (about 1000 x with the screen still; a finger on the screen always runs at real time). Power cycle restarts the firmware with the flash kept; New board erases it.

## Building

`wasm/build.sh` rebuilds `ipc-wasm.js` from `firmware/` after a firmware change. It needs zig 0.13 for its WebAssembly C library (`pip install ziglang==0.13.0`; any OS) and a shell. The firmware sources are used as they are, except sed lines for fixed hardware addresses in `stepper3.c` (NVIC, PRIMASK); the script stops if those lines change. `main.c` is compiled in whole and `sim_run()` repeats its loop body (`wasm/sim_main.c`).

The stand-ins are in `wasm/`: `include/` (register structs for GPIO, RCC, TIM7, ADC1, RTC; LVGL configuration = the firmware's heavy `lv_conf.h` without the CCM section), `sim_main.c` (registers, timer, flash, USB serial, display, touch, the page's calls), `sim_clicks.c` (`clicks.c` with readings from the bench), `bench.c` (flow line, motor, switch, Druck, supply).

## Tests

- `node simulator/tests/run.js [--png dir]`: bench scenarios against the build: boot and bundled firmware-version checks, homing from the screen, Set as %, MANUAL move prerequisites, independent homing Return, motor power loss, tuning edits and saves in AUTO, F-1 to F-3, W-1 and W-2, a pump stopped for 32 min without AUTO ending, a hot chip without an alarm, settings and saved position over a power cycle, a 50-day run past the 49.7 day tick wrap, the backlight, and the remote command set's reply rules and error codes.
- `node simulator/tests/remote-only.js`: the text port's command set: unrecognised commands answer ERR 100 with Network control On (ERR 211 with it Off), homing on the screen, then the valve and AUTO from the link.
- `node simulator/tests/tcp-board.js [port] [speed] [--remote-off] [--off]`: serves the simulated board on a TCP port (8888 by default), homed and in MANUAL at 5 % open, with KP 4.00 and TI 0.25 in every PID zone. Homing uses the `sim_home` test hook, as the screen does; startup fails if homing or the move fails. Network control is On unless `--remote-off` is passed. `--off` then shuts it down (the valve closes to 0 %, then OFF), as after Shut down on the screen, so MODE AUTO, MODE MANUAL and VALVE SET answer ERR 213. Pass port 0 to request an available local port.
- `node simulator/tests/tcp-board-test.js`: starts that helper with remote writes On and Off, and checks its homed 5 % startup, query replies, setpoint writes and STOP refusals.
- `node simulator/tests/page.js [shot.png]`: the page in Chromium (Playwright): taps, homing, a console command, settings kept over a reload.
- `node simulator/tests/text-fit.js [--depth N] [--verbose]`: every text on the screen fits its box. It taps through every screen, tab, popup and keypad reachable in six taps from twelve starting states (not homed, MANUAL, AUTO, each fault, the W-1 question, the start-up question, off, readings high, every setting at the top and bottom of its range with a long address), types the longest entry and Set into every keypad, and shows every message, question and keypad refusal in the source with long values filled in. A label fails if its text is wider than its box, wraps past its box, is cut by the card or button it is in, or comes within 3 px of the label or object beside it (`wasm/textfit.c`); a status line fails at more than two lines on the strip. About two minutes.
- `node simulator/tests/label-jitter.js`: no label moves while its text stays the same, and no time of day goes back. It samples every screen reachable in three taps from Home every 0.1 s for 6 s, in MANUAL with the pressure held near 0.95, 6, 10, 14 and 99.95 mbar and in AUTO at 1, 6, 10 and 14 mbar (the digit and zone edges). Labels only: the Events rows and the Trend are drawn. About 15 s.

## Not simulated

- A move timing out (a SYS event) cannot happen by itself: the step interrupt is never late here. The tests provoke it with bench value 24 (`sim_set(24, 1)`: the step interrupt does not run), which the page has no control for.
- Ethernet and Modbus TCP (the host build has no network; Diagnostics reads Network "No client"), the mikroBootloader, the watchdog's timing (an LVGL assert stops the page and restarts the firmware with reset reason watchdog).
- Real speed: redraw times are the browser's, not the STM32's. The simulator has the board's 64 KB LVGL heap.
- The Druck and Power Monitor are models. The flow line is a physics model, not a fit to a cryostat: its pressures, times and noise differ from a real one.

## Page controls

- Network control: the board's switch on Menu > System (On at power-up from a new board, saved as it is set). Turned Off on the screen, the page's console answers set commands with ERR 211. `boot(flash, { remote: false })` in the node tests turns it Off.
- Second finger, for the two-finger Limit override on Menu > Motor > Jog: on a touchscreen, put a second finger down; with a mouse, Shift+click holds a press down until the next Shift+click, and a plain click is then the second finger. The node tests use `sim_touch2(x, y, down)`.
