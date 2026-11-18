# iPC simulator

The simulator runs the controller's firmware in a web browser, over a simulated bench.

`ipc-wasm.js` is the firmware in `firmware/` compiled to WebAssembly. The screens, messages, fault numbers, settings and behaviour are the board's own code, not a copy of it. Only the hardware underneath is simulated. The bundle is rebuilt with every firmware version, so it matches the firmware beside it.

## Contents

- [Use](#use)
- [What is simulated](#what-is-simulated)
- [The flow line](#the-flow-line)
- [Not simulated](#not-simulated)
- [Build](#build)
- [Tests](#tests)

## Use

Open `index.html` in a browser, from a file or a web server. Nothing needs building.

| Control on the page | What it does |
| --- | --- |
| Screen | The 480 x 272 touchscreen. Use the mouse or a finger |
| Console under the screen | The text port (port 8888). The page shows each line sent; the board does not echo |
| Bench panel | Switches the helium supply and the rotary pump, unplugs the Druck or the Power Monitor Click, lowers the excitation, sets the motor supply, slips the needle, breaks or sticks the limit switch, heats the controller and turns the needle by hand |
| Speed | Real time, 10 x, 60 x, or as fast as the computer allows (about 1000 x with the screen still). A finger on the screen always runs at real time |
| Power cycle | Restarts the firmware and keeps the flash |
| New board | Restarts the firmware and erases the flash |

**Second finger.** The Limit override on Menu > Motor > Jog needs two fingers. On a touchscreen, put a second finger down. With a mouse, Shift+click holds a press down until the next Shift+click, and a plain click is then the second finger.

**Network control.** This is the board's own switch on Menu > System. It is On on a new board and saved as it is set. While it is Off, the console answers set commands with `ERR 211`.

## What is simulated

| On the board | In the simulator |
| --- | --- |
| Display and touch panel | A 480 x 272 canvas. A press lasts at least 80 ms |
| Stepper 3 Click outputs and the TIM7 step interrupt | `stepper3.c` unchanged, with its timer run per simulated microsecond. The four outputs turn a simulated motor, so Direction (Motor > Settings) acts as on the board. The coil order is 0-2-1-3 |
| Needle valve and limit switch | A needle with 16512 steps a turn that seats 30 steps past the switch. Switch faults: broken wire, stuck, trips early |
| Druck transducer through the Power Monitor Click | The pressure of the flow line, read through the 5 % input loading. Open to room air for calibration |
| Boost 10 Click, motor 12 V supply, Power Monitor Click temperature | Bench controls: excitation, supply volts, controller temperature. The motor stalls without its supply |
| Serial flash (settings, saved position) | 8 MB in memory, kept in the browser between visits |
| Port 8888 | The console under the screen |
| Real-time clock | The computer's clock at power-up |
| Backlight | Dims to 10 % after 10 min with no touch and goes dark 30 s into OFF, as on the board |

## The flow line

The flow line is a physics model: the helium bath, the needle valve, the pot and the rotary pump. It is the same model as `tools/ctrl-test/model.c`.

| Valve, % open | 20 | 30 | 40 | 50 | 60 | 80 | 100 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Pressure, mbar | 0.8 | 2.1 | 3.8 | 5.4 | 6.9 | 9.8 | 12.5 |

This is far less pressure per % open than a cryostat gives, so AUTO with the factory PID gains is very slow on it. KP 4.00 and TI 0.25 in every zone suit it. The page says so under Cryostat, and the tests set those gains wherever they test something other than the gains.

## Not simulated

- **Ethernet and Modbus TCP.** The build is the host build (`IPC_LINK_ETHERNET 0`), which has no network. Diagnostics reads Network "No client".
- **The mikroBootloader and the watchdog's timing.** An LVGL assert stops the page and restarts the firmware with the reset reason watchdog.
- **Real speed.** Redraw times are the browser's, not the STM32's. The LVGL heap is the board's 64 KB.
- **A late step interrupt.** A move cannot time out by itself here. The tests provoke it with `sim_set(24, 1)`, which stops the step interrupt; the page has no control for it.
- **A real cryostat.** The Druck, the Power Monitor Click and the flow line are models. Their pressures, times and noise differ from a real cryostat's.

## Build

```
pip install ziglang==0.13.0
simulator/wasm/build.sh
```

`wasm/build.sh` rebuilds `ipc-wasm.js` from `firmware/`. Run it after every firmware change. It needs zig 0.13, for its WebAssembly C library, and a shell.

The firmware sources are compiled as they are. The only exception is a few `sed` lines for fixed hardware addresses in `stepper3.c` (NVIC, PRIMASK); the script stops if those lines change. `main.c` is compiled whole, and `sim_run()` repeats its loop body.

The stand-ins for the hardware are in `wasm/`:

| File | Holds |
| --- | --- |
| `include/` | Register structs for GPIO, RCC, TIM7, ADC1 and RTC, and the LVGL configuration (the firmware's `lv_conf.h` without the CCM section) |
| `sim_main.c` | Registers, timer, flash, display, touch, and the calls the page makes |
| `sim_clicks.c` | `clicks.c` with its readings taken from the bench model |
| `bench.c` | Flow line, motor, limit switch, Druck and supplies |
| `textfit.c` | The text-fit measurements used by `tests/text-fit.js` |

## Tests

All tests run in node from the repository root. `page.js` also needs Playwright with Chromium.

| Command | Checks | Time |
| --- | --- | --- |
| `node simulator/tests/run.js [--png dir]` | Bench scenarios against the build (listed below). `--png` saves a capture of the screen at each step | 15 s |
| `node simulator/tests/remote-only.js` | The text port's command set: unrecognised commands answer `ERR 100` with Network control On and `ERR 211` with it Off; homing on the screen, then the valve and AUTO from the link | 1 s |
| `node simulator/tests/tcp-board-test.js` | Starts `tcp-board.js` with Network control On and Off, and checks its start-up, query replies, setpoint writes and STOP refusals | 1 s |
| `node simulator/tests/page.js [shot.png]` | The page in Chromium: taps, homing, a console command, settings kept over a reload | 15 s |
| `node simulator/tests/text-fit.js [--depth N] [--verbose]` | Every text on the screen fits its box | 2 min |
| `node simulator/tests/label-jitter.js` | No label moves while its text stays the same, and no time of day goes back | 15 s |

**`run.js` scenarios:** boot and the bundled firmware version, homing from the screen, Set as %, what MANUAL moves need, the homing Return, motor power loss, tuning edits and saves in AUTO, faults F-1 to F-3, warnings W-1 and W-2, a pump stopped for 32 min without AUTO ending, a hot chip without an alarm, settings and the saved position over a power cycle, a 50-day run past the 49.7 day tick wrap, the backlight, and the text port's reply rules and error codes.

**`text-fit.js`** taps through every screen, tab, popup and keypad reachable in six taps from twelve starting states: not homed, MANUAL, AUTO, each fault, the W-1 question, the start-up question, off, high readings, and every setting at the top and bottom of its range with a long address. It types the longest entry into every keypad and shows every message, question and keypad refusal in the source with long values filled in. A label fails if its text is wider than its box, wraps past its box, is cut by the card or button it is in, or comes within 3 px of its neighbour. A status line fails at more than two lines on the strip.

**`label-jitter.js`** samples every screen reachable in three taps from Home, every 0.1 s for 6 s. It runs in MANUAL with the pressure held near 0.95, 6, 10, 14 and 99.95 mbar, and in AUTO at 1, 6, 10 and 14 mbar, which are the digit and zone edges. It checks labels only; the Events rows and the Trend are drawn, not labels.

### A simulated board on a TCP port

```
node simulator/tests/tcp-board.js [port] [speed] [--remote-off] [--off]
```

This serves the simulated board on a TCP port, 8888 by default, for a program that talks to the text port. Pass port 0 for any free port.

- The board starts homed, in MANUAL at 5 % open, with KP 4.00 and TI 0.25 in every PID zone.
- Start-up fails if the homing or the first move fails.
- `--remote-off` starts it with Network control Off.
- `--off` shuts it down after start-up: the valve closes to 0 % and the controller is OFF, so `MODE AUTO`, `MODE MANUAL` and `VALVE SET` answer `ERR 213`.
