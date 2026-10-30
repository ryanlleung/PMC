# PMC

Pressure-based needle-valve controller for helium-flow cryostats, developed during a 12-week graduate placement in a sample environment group.

## Context

Oxford Instruments cryostats (CF-10, CF-11, HM-2, AVM-1, AVM-2) draw helium through a needle valve, driven by a geared stepper motor and regulated today by a Mercury iTC. The iTC holds the pressure measured by a Druck transducer on the pumping line. Pressure is a rough proxy for the helium flow; the flow itself is not measured. This project replaces the iTC's valve control with a standalone controller that keeps the existing Druck transducer and stepper valve.

How it is used: operators set a target pressure for each cryostat from experience (typically 1 to 25 mbar) and switch between MANUAL and AUTO. They do not tune the control loop. They watch the sample temperature on another controller, so the pressure trend on this one matters.

Constraints: bought modules only (no PCB design), no PLC, Modbus TCP and a small touchscreen, 0.1 mbar resolution. The valve's limit switch is the closing stop and is read in firmware; there is no hardware interlock.

Two hardware options are being built and compared; neither is final:

- **Option C (this repository):** a MikroE Mikromedia 4 (STM32F407) touchscreen board with Click boards for the stepper driver, the Druck excitation and the Druck reading. All control and the screens are in this firmware.
- **Option A:** a Eurotherm EPC3008 controller with an Applied Motion STF06-R stepper drive in Modbus position mode. The EPC runs the PID and writes the valve target to the drive over RS-485. Not in this repository.

`simulator/` holds the simulator: this firmware compiled to WebAssembly, run in a browser over a simulated bench (`simulator/README.md`).

`secop/` holds a Frappy (SECoP) driver for the text port, so a SECoP server can read the pressure and set the target, AUTO and the valve (`secop/README.md`). It needs the remote command set (`SP`, `MODE AUTO`, `STOP`, ...), planned for the same release.

## This repository

This repository holds the Option C prototype: firmware for the MikroE Mikromedia 4 (STM32F407ZG) with Click boards, built in NECTO Studio 7 (mikroSDK, LVGL 9.4). The firmware is in `InitialTesting/`.

Firmware version: `PMC_FW_VERSION` in `InitialTesting/pmc_config.h` (0.10.11 at the time of writing). Commit titles start with the version they build.

## Status

The simulator is being ported to the firmware in stages, one version and one pull request each, each passed on the bench before the next:

| Stage | Version | Content | State |
| --- | --- | --- | --- |
| 0 | 0.3.x | Baseline: Druck reading, bench calibration, limit switch | Merged (#10) |
| 1 | 0.4.x | Motion: absolute moves with retarget, ramped and immediate stop, F-5 timeout | Merged |
| 2 | 0.5.x | Valve layer: modes, homing, % position, STOP, faults, event log | Merged |
| 3 | 0.6.x | Settings store in serial flash, two sectors, Previous save | Merged |
| 4 | 0.7.x | Screen framework (header, strip, STOP) and Home | Merged |
| 5 | 0.8.x | MANUAL operator screens | Merged |
| 6 | 0.9.x | Settings screens, Calibrate sensor, saved valve position | Merged (#13) |
| 7 | 0.10.x | Trend and the Home sparkline | Merged (#14) |
| 8 | 0.11.x | AUTO (pressure control loop) | Draft PR, bench test due |
| 9 | 0.12.0 | Annunciation (blink, acknowledge, buzzer) | Not started |


## Hardware as assembled

| Part | What it does | Connection |
| --- | --- | --- |
| Mikromedia 4 for STM32F4 (STM32F407ZG, 168 MHz, 1 MB flash, 192 KB RAM) | Controller, 4.3 in 480x272 capacitive touchscreen (SSD1963 display controller, touch over I2C) | 12 V terminal, USB-C |
| Socket S1: limit switch (Adapter Click and Pressure 21 Click on order) | One NC dry contact to GND, opens at the closed end of travel | Socket TX, PD5 (internal pull-up): Adapter IDC10 pad 8, printed RX, because the Adapter labels its UART pads from the Click's side (pad 7, printed TX, is the socket's RX, PA3). GND on pad 10 |
| mikroBUS shield, socket S2: Stepper 3 Click (ULN2003) | Drives the unipolar six-lead valve stepper (PK22, 344:1 gearbox), 12 V | AN PB0, RST PC3, CS PA15, PWM PD13 |
| Socket S3: Boost 10 Click (LT8337, TPL0501 digipot) | Raises 5 V to about 10 V excitation for the Druck. Never feed it 12 V | SPI1 PA5/PA6/PB5, CS PF8, PG PG2 |
| Socket S4: Power Monitor Click (INA228) | Reads the Druck output (shunt input) and the excitation (VBUS), ±163.84 mV range | I2C2 PF1/PF0, address 0x4A |
| Druck UNIK 5000 PDCR50G1, 15 psia absolute, 10 mV/V | Pressure transducer | M12: pin 1 +Supply to Boost 10 VOUT and INA228 VBUS; pin 2 +Out to IN+; pin 4 -Out to IN-; pin 3 -Supply to common GND |
| On-board SST26VF064B (8 MB) | Settings, calibration and saved valve position | SPI2 bit-banged: CS PB11, SCK PB13, MISO PB14, MOSI PB15 |
| On-board RTC, 32.768 kHz crystal | Time stamps; survives a reset (and a power-off with a cell in BAT1) | |
| On-board LAN8720A | Ethernet link (build option) | RMII |

Power: the board takes 12 V on its terminal (10.6 to 14 V; priority 12 V, then USB-C, then Li-Po; SW1 on). The motor supply is regulated 12 V from a PoE splitter (Planet IPOE-162S). The Stepper 3 Click has no current limit, so the motor supply must stay at 12 V. Socket pin details: `InitialTesting/clicks.h`, `limit.h`, `extflash.h`, and the comments at the top of each driver.

## Firmware structure

`main.c` initialises everything (version and reset cause, clock, LVGL and display, calibration, Clicks, limit switch, valve, settings, saved position, screens, PC link), starts the independent watchdog (about 3 to 4 s), then runs one loop with no RTOS:

1. PC link, touch (every 5 ms), clock, limit switch, stepper and valve tasks, saved position.
2. Once a second: `sense_second()` reads the INA228, converts to pressure, updates the faults and the trend, and sends the DATA line and any changed status line.
3. One command line from the PC, if any, offered to each module's `*_command()` in turn.
4. `lv_timer_handler()` (screens refresh at 2 Hz, the fast parts at 10 Hz), the watchdog kick, 1 ms delay.

Steps are timed by TIM7 interrupts, so the motor runs smoothly while the screen draws.

| File | What it does |
| --- | --- |
| `pmc_config.h` | Build options: link (USB or Ethernet), version, Stepper 3 socket, limit input pin and polarity |
| `clicks.c` | Boost 10 excitation set-up and 11.5 V trip, INA228 reads, Druck plausibility, pressure from the cal |
| `cal.c` | Druck calibration table: ratio (mV/V) against pressure, linear between points; bench two-point (CAL VAC, CAL AIR) |
| `sense.c` | The once-a-second read, DATA and status lines |
| `limit.c` | Limit switch input: a trip counts at once, the release is debounced (20 ms) |
| `stepper3.c` | Step sequences (wave, full, half), ramps, absolute moves with retarget, the limit stop, F-5 timeout |
| `valve.c` | Modes (NOT HOMED, HOMING, MANUAL, AUTO), homing, % position, STOP, fault latches, the event log (100 kept), runs AUTO |
| `ctrl.c` | AUTO control step, ported from the simulator: PI, anti-windup, deadband, response monitor (F-6) and W-2 checks (no hardware calls; `tools/ctrl-test` runs it on a PC) |
| `valvepos.c` | Saves the step count at standstill and every 2 s while moving, so a restart can offer it |
| `cfg.c` | Settings record in serial flash, two sectors in turn, CRC32, read back before it counts |
| `trend.c` | Trend rings in RAM (11.5 KB): 600 one-second samples and 360 ten-second means, on fixed one-second slots; slots the loop missed go in as no-data samples. `TREND DUMP` is sent a line at a time as the link has room |
| `extflash.c` | SST26 serial flash driver |
| `rtclock.c`, `sysinfo.c` | Clock (`TIME?`, `TIME SET`); version and reset cause (`VER?`) |
| `link.c`, `usb_serial.c`, `ethlink.c`, `eth_hw.c` | PC text link over USB CDC or TCP |
| `modbus.c`, `mbmap.c` | Modbus TCP server on port 502 in the Ethernet build: the protocol (FC1, FC3, FC5, FC16) and the register map (`docs/modbus-register-map.md`; `tools/modbus-test` runs both on a PC) |
| `ui.c` | Screen framework: header, status strip, STOP, buttons, cards, keypad, pick lists, dialogs, text view |
| `ui_home.c` | Home and Menu |
| `ui_valve.c` | Valve, Home valve, Faults and log, Diagnostics |
| `ui_settings.c` | Settings pages and Calibrate sensor |
| `ui_trend.c` | Trend page and the Home sparkline (drawn on a draw event, no lv_chart) |
| `motor_screen.c` | Settings > Motor override: a framework screen (header, strip, STOP) with every MOT move and setting |
| `driver/` | Display and touch ports for LVGL; `display_px.c` is the pixel loop |
| `fixed.c` | Fixed-point text and parsing (lv_snprintf has no float) |

## Operation

- **W-3 controller hot** (0.11.36): the Power Monitor die temperature at or above 55 C shows "W-3 Controller hot" on the strip on every screen until it falls below 50 C. A warning only: not latched, blocks nothing; both changes are logged.
- **Nothing moves at power-up.** The valve is NOT HOMED until it is homed, or until the operator sets the position: Home and Set position are kept apart on every screen. Home finds the switch and takes 0 % as the return above it; Set position takes it from the operator (closed, a typed %, or the position saved before the restart), not checked against the switch. At start-up with a saved position the screen asks "Home / Set position / Use X %"; it is never used on its own. Menu > Valve position has a Home tab and a Set position tab, and a position set rather than homed shows on the status line ("34.20 %, set by hand").
- **Homing** (Home valve, or `VALVE CAL`): if the switch is pressed, move off it first (F-4 if it stays pressed); 1 fast close to the switch (F-3 if not found within 1.1 x full travel); 2 back off; 3 slow close onto the switch (the seat); 4 return off the switch (0.12.11; by the back-off until 0.13.5, by its own Return setting from 0.13.6, Ryan 28 Oct, default 400 steps, and older saves load it as their back-off) with the switch released (F-4 if not). That last point is 0 % (0.12.12, Ryan 26 Oct), so the seat reads -400 steps, about -0.6 %, with the defaults. Set as 0 % (`VALVE CLOSED`) and Set as 0 % in the switch question give the seat the same count. STOP cancels homing and keeps the earlier homing.
- **MANUAL**: on the Valve page, tap the Go to box (white, blue underline and chevron when it takes a tap) to type a %; hold Close or Open; 1 % and 5 % steps; Fully close and Fully open. In AUTO the Go to box shows, greyed, the position AUTO is sending the valve to, and a tap asks to switch to MANUAL. Position is in % of POS_MAX steps (66048, 4 turns, by default).
- **Limit switch** is the closing stop, read in firmware before every closing step; there is no hardware interlock (decided). A trip during a normal move stops it and asks "Set as 0 % / Skip" (W-1); the count is never zeroed on its own. Limit override (Motor override screen) is a two-finger deadman from 0.13.7: held alone it does nothing; while it is held, Close pressed with another finger closes past an open switch, and the move stops when either finger lifts.
- **Motor settings**: changing the direction or coil order, or zeroing the motor count (`MOT ZERO`, the motor screen, a loaded record), clears the homing; the valve must be homed again. A step mode change keeps it: travel, back-off, return, clearance, offset and homing speeds are in full steps and scaled to half steps in HALF.
- **Saved position**: written 2 s after the valve stops. Before a move starts the start count is written as "saved while moving" (read back and checked, one retry; if it cannot be written the move is refused), then the count again every 2 s while it moves, only when it has changed. A power cut mid-move therefore loses at most about 2 s of travel, and start-up says the position was saved while moving and how far out it may be. Losing calibration withdraws the entry. The log takes one sector erase per 256 entries over 16 sectors, about 26 years of continuous moving at 1 entry every 2 s. At start-up every slot of the log is read, so an entry after a failed write is still found.
- **AUTO** (Start AUTO, or `MODE AUTO` over the link): needs the valve homed, no fault, a valid reading and a setpoint. The first Start AUTO asks for the setpoint and Set starts AUTO; later ones confirm the setpoint kept or offer New setpoint. The setpoint changes from the Setpoint box (over 5 mbar asks first; the screens say setpoint from 0.13.3, the remote commands keep their names). Once a second, after the reading, `ctrl.c` works out the valve command: PI in position form (D on the measurement, off), bumpless entry from where the valve is going, no integration past 100 % or AUTO min, while the switch blocks closing, or while the valve is more than 3000 steps short of the last command, and commands inside the deadband held. Each move runs at MOT RATE. A move the opposite way to the last one is held until the command is 0.1 % of travel from the valve (0.12.12), so the reading's noise no longer turns the motor round every second inside the drive's backlash. The status line reads Regulating to X, In band (within the band) and Stable (60 s in the band). Switch to MANUAL ramps the valve down and holds it where it stops; from 0.13.4 it does not ask first (a tap on Go to in AUTO still asks).
- **AUTO checks**: a response monitor over 30 s windows (the valve position through a 40 s lag, moves under 1000 steps not judged): F-6 after 5 windows with no pressure response. A wrong-way change counts as a response (the wrong-way faults and the above-range fault were removed in 0.11.20: in the flow-line model a lost helium supply then shows as F-6 after about 5 min, a stopped pump as W-2 after 30 min). W-2 after 1800 s outside the band stops the valve and switches to MANUAL, the valve idle; the strip shows W-2 until OK or the next Start AUTO (no PAUSED state since 0.11.25). Any fault in AUTO stops the valve and drops to MANUAL, holding where it stopped. Starting AUTO is still refused with the pressure above the sensor range.
- **STOP** stops and holds the valve with no latch. In AUTO it also drops to MANUAL (logged as STOP).
- **Faults** latch until Reset fault (or `ALRM RESET`), which clears only those whose cause has gone: F-1 no pressure signal, F-2 excitation outside 9.5 to 10.5 V, F-3 switch not found while homing, F-4 switch still pressed after moving off it, F-5 motion timeout, F-6 no pressure response (AUTO). The number order is the priority order. F-3 to F-6 block moves until reset; F-1 and F-2 do not.

## Screens

480x272, after the simulator (`simulator/`, ISA-101 style: grey for normal, colour only for abnormal states). Every screen has a header (Back and Home buttons, the title centred, the mode badge on the right, as wide as the button column: filled white in AUTO, outlined white in other modes, NOT HOMED with no outline, red with a fault), a status strip and STOP on the bottom row. Under a popup the strip stays, updating and greyed. A keypad is its own page: the header shows its title and a working Home (0.13.18). On Home and the operator screens the strip shows the status line (a grey icon: the valve symbol when idle, an arrow while moving, a loop while AUTO regulates, a target once Stable; Moving to X %, or in AUTO Regulating to / In band / Stable at the target; an idle valve shows the valve symbol and "Valve idle" (0.13.16), and how its position was set (entered by hand, from the last save) is on Home valve) with the 5 min pressure sparkline on its right; prompts, faults and messages take the whole strip. Sparklines and Trend draw each time slot through its first sample, the two either side of its largest change and its last, so a step shows upright.

From power-up to Home a start-up screen shows (0.13.3): the panel memory is cleared to white while the display is off, then, from 0.13.13, the MercShore iPC logo on white (`ui_img_logo.c`, 334x168 RGB565, 110 KB of flash, made from `tools/images/mercshore-ipc-logo.png` by `tools/images/mkimg.py`), and a rounded blue bar that fills as each part of the board starts (no text from 0.13.14), then Home after about 2 s; the firmware version and build date are in the corner. 0.13.10 to 0.13.12 showed the checks as an old PC's self test on black.

- **Home** (title "<cryostat name> pressure controller", for example "CF-11 pressure controller", on the left): Pressure tile (40 px reading in mbar, log bar labelled 0.1, 1, 10, 100 and 1k with the end lines the same distance in from each end, Setpoint box) and Valve tile (40 px reading in %, bar, Go to box), the mode button (Home valve / Start AUTO / Switch to MANUAL; from 0.13.1 Start AUTO is greyed while a fault is latched or the motor supply is off, and Switch to MANUAL is not filled; from 0.13.3 it is white with a blue edge and blue word, Start AUTO the other way round), Valve, Menu, the pressure card (a tap on the sparkline opens Trend).
- **Menu**: Records (Trend, Faults and log), Calibrate (Valve position, Transducer), System (Diagnostics, Settings).
- **Valve**: manual moves and the valve's state. **Home valve**: homing state, motor step count, limit switch, the four steps and the time taken. **Faults and log**: latched faults (tap for detail and help) and the event log. **Diagnostics**: readings in three rows (pressure sensor: signal, before cal and the Pressure 21 room reading; valve motor: steps, limit switch, last move; controller: board temperature, excitation and how long the last move took, from 0.14.5), with Home's status line in the strip from 0.13.4; from 0.13.2 the firmware version, the address and the MAC are on Settings > System setup, and from 0.13.5 Restart is on Settings beside Motor override. From 0.12.19 the header shows Unsaved (a save icon from 0.13.8), left of the mode badge, on every screen while a setting in use differs from the newest save; tapping it opens Review and save.
- **Trend**: pressure (left axis), the target while in AUTO (dashed), and valve (right axis, 0 to 100 %) over 1, 2, 5, 10, 30 min or 1 hour. Each point is the mean of a fixed slot of time counted from start-up, so the line keeps its shape as it scrolls.
- **Settings**: System setup (Name and network to 0.13.6, System to 0.13.18): Cryostat name (a keyboard; up to 12 characters, in use at once and unsaved until Save settings) and Fallback address (0.12.13: address and mask on a number pad, used when no DHCP server answers, after Save settings and a restart; Default goes back to the build's `PMC_FALLBACK_IP`), Remote control (0.13.4: Off, the network can only read; On, set commands on port 8888 and Modbus writes are taken; a toggle switch from 0.13.7, On by default and in records from before 0.13.4 (Off to 0.13.6); a press turns it On or Off at once and logs a NET event, Save settings keeps it through a restart), with the address in use, the MAC and the firmware version with its build date; Restart (asks first, stops the valve, restarts after 2.5 s so the position is saved) is filled dark beside Motor override on the Settings page from 0.13.5; Choose sensor (up to four Druck sensors, each with its own cal; one in use), Valve setup (travel, direction; homing back-off, return and speeds; drive rates; from 0.13.6 a tab shows its values at once), Control tuning (for AUTO), Review and save, Load settings (last save, previous save, factory), Motor override in three tabs: Move (one row of choices: Hold to run, moving while Close or Open is pressed and on by default, or a step size 1 to 10k; then Limit override, Close, Open and Set zero; the position card shows output turns from count 0. From 0.13.7 Limit override is a two-finger deadman: held alone it does nothing; while it is held, Close pressed with a second finger closes past the switch and Open opens, by the move choice; lifting either finger stops the move, a close past the switch at once; the strip says so while it is held), Speed (run, start and accel rates, and the time for a full open or close at those rates) and Coils (mode, hold torque, coils off, coil order, direction). Order, direction and Set zero ask first while the valve is homed, as they clear the homing; a mode change keeps it (valve settings are in full steps, the count is rescaled). The position shows on every tab; the strip reads the motor state and the limit switch. Edits are drafts until Save settings, which checks them together and saves all or nothing.
- **Calibrate sensor**: the bench two-point cal from the screen (vacuum, then air at the Pressure 21 room reading or a typed barometer reading).

Display formats: pressure 1 decimal, valve % and keypad 2 decimals, dates DD/MM/YY. Fonts are Montserrat Medium 12, 16 and 26 px (`ui_font_*.c`, converted with lv_font_conv, ASCII plus ± ° µ – •), and 40 px digits only for the Home readings (`ui_font_40.c`). The display is driven over the SSD1963's 16-bit 8080 bus, bit-banged on GPIOE (WR on PF11); touch is read over I2C every 5 ms.

LVGL heap: 64 KB in CCM plus a 24 KB second pool in main RAM. `PERF?` reports loop and draw times and heap use.

## Pressure measurement

The Druck output is ratiometric, so the firmware uses the ratio of signal to excitation (mV/V), read by the INA228 with 128 averages over 600 ms (30 cycles of 50 Hz mains), once a second. `cal.c` turns the ratio into pressure. The normal cal is the bench two-point: a pumped-down vacuum point (`CAL VAC`, or Calibrate sensor) and an air point at a barometer reading (`CAL AIR`). Both are read live through the INA228 as wired, so the INA228's loading of the Druck output (the reading is about 5 % low uncalibrated) and its gain and offset sit inside the cal. With no cal saved, the nominal 0 to 100 mV at 10 V is used and the screen says so. The display shows 0.1 mbar; accuracy is set by the Druck (±0.04 % FS, ±0.41 mbar) and the cal points.

## Storage

Serial flash (survives reflashing; neither the application nor mikroBootloader erases it):

| Address | Holds |
| --- | --- |
| 0x7EC000 to 0x7FBFFF | Saved valve position, a log of 16-byte entries over sixteen 4 KB sectors (one erase per 256 entries) |
| 0x7FD000 and 0x7FE000 | Settings record (version 3), written to the two sectors in turn |
| 0x7FF000 | Old stand-alone cal record (0.3.30 to 0.5.x), read only if no settings record exists |

The trend and the event log are in RAM and empty after a restart.

## Build and flash

- `InitialTesting/pmc_config.h` sets the PC link: `PMC_LINK_ETHERNET 1` (the default from 0.12.18) gives TCP port 8888 (5000 to 0.12.21; DHCP, or the fallback address if no DHCP server answers: the one saved from Settings > System setup, else `PMC_FALLBACK_IP`, 169.254.10.50), and `0` gives the USB COM port. The port takes the remote command set, as USB does; `PMC_LINK_WRITE 0` makes it read only (queries; set commands answer ERR 211). With 1, set commands still need Settings > System setup > Remote control On (0.13.4, `PMC_REMOTE_SWITCH`, on in the Ethernet build and the simulator): On by default from 0.13.7; while it is Off they answer ERR 211, STOP included, and so do the engineering commands of a bench build. `PMC_ENG_COMMANDS 1` makes a bench build with the engineering commands as well. The Ethernet build also runs a Modbus TCP server on port 502 (`docs/modbus-register-map.md`), with writes on (`PMC_MODBUS_WRITE 1`, from 0.12.6) while Remote control is On (0.13.4); `0` makes it read only whatever the switch says. The two links can't run together, because RMII needs 50 MHz from MCO1 and USB needs 48 MHz from the same PLL.
- The linker script `.meproject/setup/FileLinker/stm32f407zg.ld` is patched so `.ccmram` is `(NOLOAD)`, which keeps the LVGL heap that lives there out of the flash image. If NECTO ever regenerates this file, re-apply the patch.
- Build Debug in NECTO. Release leaves the display blank: MikroE's prebuilt Release `lib_ssd1963.a` was compiled with a non-volatile GPIO register struct, so `ssd1963_write_command` loses its WR low and the SSD1963 never latches a command (bench, 0.3.33; disassembly, 0.8.9). From 0.8.10 (pixel loop from 0.8.11) the Debug build keeps the Debug SDK libraries and the hardware drivers at -O0 but compiles LVGL, the pixel loop `driver/display_px.c` and `ui*.c` at -O2 (`driver/display_lvgl.c` must stay -O0: it inlines the SDK's WR_LOW/WR_HIGH) (`InitialTesting/CMakeLists.txt`), so screens draw faster. About 630 KB of flash (0.10.4).
- NECTO's build folders (`build-MM4/`, `build-Default Configuration/`, ...), `compile_commands.json` and the clangd index in `.cache/` are not tracked, so a NECTO build no longer blocks `git pull`. The one exception is `build-MM4/generated/` (screen code from `main_screen.mscr`), which `tools/ui-preview` compiles. NECTO also rewrites `.meproject/config.json` (it holds machine paths, but also the board and MCU selection, so it stays tracked): before a pull, `git stash` or `git checkout -- InitialTesting/.meproject/config.json`.
- After `CMakeLists.txt` changes (new files), Clean before building in NECTO.
- Flash over USB-C with mikroBootloader (no debugger). Check `*IDN?` after flashing (the version is its last field).

## PC link commands

From 0.13.0 the text link takes the remote command set only: what an operator can use and judge away from the cryostat. Homing, calibration, settings, tuning, saving and the motor are on the touch screen. The full description for a driver writer (for example a Frappy module) is the "mm4 Remote Commands" page; `InitialTesting/remote.c` is the code.

Connect to TCP port 8888 in the Ethernet build (5000 up to 0.12.21; e.g. PuTTY in Raw mode), or the USB COM port in the USB build (any baud rate, it is virtual). One client at a time; a new connection replaces the old. Send upper case ASCII, a line ending in LF or CR LF, at most 96 characters. Replies end in CR LF.

- No echo, and nothing is sent unasked: faults, warnings and switch changes go to the event log (`EVNT?`) and show in `ALRM?` and `MODE?`.
- A query (ending in `?`) replies one line starting with its own words, the value then the unit: `PRES 4.625 mbar`, `VALV 34.12 %`, `VALV NONE` when there is no value. Pressure 3 dp, target 1 dp, valve 2 dp, room pressure 2 dp.
- A set command replies nothing when it is accepted, and `ERR <code> <text>` when refused. End a line that sets something with a query (`PRES SP 5;MODE AUTO;MODE?`) to always get one line back.
- Commands can share a line with `;`: they run in order, the query replies come back on one line joined by `;`, and the first `ERR` ends the line. `EVNT?` is the only multi-line reply (`EVNT <count>`, then that many lines) and must be on a line of its own.

| Command | Reply or action |
| --- | --- |
| `PRES SP <mbar>` | Target, 0 to the sensor range, kept to 0.1 mbar. Does not start AUTO |
| `MODE AUTO` / `MODE MANUAL` | Start regulating after the entry checks / stop regulating, the valve stays where it is |
| `VALV SET <%>` | Move the valve, 0 to 100 % in 0.1 steps; MANUAL only, valve homed |
| `STOP` | Stop the valve now; in AUTO it also ends AUTO. Cancels homing. The only stop |
| `ALRM RESET` / `ALRM OK` | Clear the latched faults whose cause has gone (ERR 210 if one is still present) / acknowledge the warning, as the screen's OK |
| `TIME SET <YYYY-MM-DD> <HH:MM:SS>` | Set the clock |
| `PRES?` | `PRES 4.625 mbar` or `PRES NONE` |
| `PRES SP?` | `PRES SP 5.0 mbar` or `PRES SP NONE` |
| `MODE?` | `MODE MANUAL`, `MODE AUTO`, `MODE HOMING` or `MODE NOT HOMED` |
| `VALV?` / `VALV MOVING?` | `VALV 34.12 %` or `VALV NONE` / `VALV MOVING 1` while moving or homing, else 0 |
| `BARO?` | `BARO 1013.25 mbar` or `BARO NONE` (Pressure 21 Click not fitted or not reading) |
| `ALRM?` | `ALRM NONE`, or the active alarms joined by `\|`, faults first: `<tag>,<0\|1>,<text>`. For a fault, 1 when `ALRM RESET` clears it now; for a warning, 1 when `ALRM OK` clears it, 0 while its cause is present (W-1 the switch question, W-3 controller hot) |
| `EVNT? [<n>]` | `EVNT <n>`, then n lines `<age s>,<tag>,<text>`, newest first; default 1, at most 100 |
| `NETW?` | `NETW DHCP\|FALLBACK,<address>,<mask>,<MAC>`; `NETW NONE` in the USB build |
| `TIME?` | `TIME 2026-10-27 11:05:00` or `TIME NONE` |
| `*IDN?` | `MercShore,iPC,<chip id>,<version>` |

Error codes: 100 unknown command (every screen-only command), 101 expected form, 102 outside range, 103 line too long, 104 `EVNT?` not on its own line, 201 homing, STOP to cancel, 202 switch to MANUAL first, 203 reset the fault first, 204 home the valve first, 205 no valid pressure, 206 set a target first, 207 target outside the sensor range, 208 pressure above the sensor range, 209 control settings not set, 210 not reset, cause still present, 211 read only over the network (Remote control Off on the screen, or `PMC_LINK_WRITE 0`), 212 the valve cannot move now (motor supply off, the limit switch, a step test running) or the clock is not running.

`tools/logger/log_druck.py` polls the set once a second, and `simulator/tests/tcp-board.js` serves the simulated board on port 8888 for trying a driver without hardware.

### Engineering commands (bench build)

A bench build (`PMC_ENG_COMMANDS 1` in `pmc_config.h`; the simulator builds this way) also takes the engineering commands below, under their old names. They reply in their own style: data lines, then one final `OK` or `ERR ...` line. The cryostat firmware (`PMC_ENG_COMMANDS 0`, the default) answers every one of them `ERR 100 Unknown command`. `HELP` lists them.

| Command | What it does |
| --- | --- |
| `HELP` | The engineering commands |
| `VER?` | Firmware version and build time, MAC and IP |
| `DATA?` / `DATA HDR?` | One CSV line of the last reading, `DATA,t_s,p_mbar,target_mbar,valve_pct,mode,alarm,ratio_mV_per_V,exc_mV,die_C,baro_mbar` / the column names |
| `SENSE?` | The Power Monitor and Druck readings now |
| `BARO STATUS?` | The Pressure 21 Click's status: reading, temperature, or why it is not read |
| `LIM?` | Valve limit switch: state, raw pin level, change count |
| `TOUCH?` | Touch controller read and error counters |
| `PERF?` | Longest main loop and display timings since the last `PERF?`, and the LVGL heap (total, used, peak since boot, biggest free block, fragmentation) |
| `TREND?` / `TREND DUMP <n>` | The trend buffer / its last n seconds |

Settings store (engineering; Settings > Review and save on the screen) (motor, valve, control tuning, up to four pressure sensors each with its own cal, the cryostat name and the save time, in one record in serial flash; version 3 from 0.10.9, a version 2 record (0.9.0 to 0.10.8) loads with the name CF-11, and a 0.6.0 to 0.8.x record still loads as one 15 psi sensor). Changes over the link are live at once and unsaved until `CFG SAVE`. At power-up the newest valid record is loaded; with none, the factory defaults (and a cal saved by firmware 0.3.30 to 0.5.x, if any):

| Command | What it does |
| --- | --- |
| `CFG?` | Both flash sectors (revision, valid or why not), what was loaded at power-up, number of unsaved settings |
| `CFG DIFF?` | Each setting that differs from the newest save |
| `CFG SAVE` | Save everything (refused in AUTO, as `CAL SAVE`). Two sectors are used in turn; a save is read back before it counts, so a failed save keeps the last good record |
| `CFG LOAD LAST` / `CFG LOAD PREVIOUS` | Load the newest / the other saved record (shows as unsaved until saved). Refused while moving or homing |
| `CFG LOAD DEFAULTS` | Factory motor, valve and tuning settings; the sensors and their cals are kept |
| `CFG NAME <text>` / `CFG NAME?` | Set / show the cryostat name in the Home title (up to 12 characters; unsaved until `CFG SAVE`) |
| `NET?` | Ethernet fallback address (used when no DHCP server answers) and the one in use since start-up |
| `NET IP <address> <mask>` / `NET IP DEFAULT` | Set the fallback address, e.g. `NET IP 192.168.1.50 255.255.255.0`, or go back to `PMC_FALLBACK_IP`. Unsaved until `CFG SAVE`, used after a restart. Refused over Ethernet so nobody cuts off their own link: set it on the screen, or over USB |
| `CFG TEST CORRUPT` / `CFG TEST ERASE` | Bench tests: spoil the newest record's CRC / erase both sectors |

Calibration (engineering; the Calibration screen on the cryostat) (Druck, through the INA228):

| Command | What it does |
| --- | --- |
| `CAL?` | Active cal, whether it is saved, and the bench points held |
| `CAL VAC [mbar]` | Capture the vacuum point over 5 readings. Default 0 mbar, range 0 to 50 mbar. Answers `OK capturing ...` at once; the result is a `CAL` event (`EVNT?`) |
| `CAL AIR [mbar]` | Capture the air point at the reference reading, 500 to 1100 mbar; without a number, at the Pressure 21 room reading averaged over the same readings (0.14.0). As `CAL VAC`, the result is a `CAL` event, then `Bench cal applied` once both points are held |
| `CAL SAVE` | Same as `CFG SAVE`: saves all settings, the active cal included (it is only in RAM until then) |
| `CAL ERASE` | Go back to nominal, erase the old 0.5.x cal record and save |
| `CAL NEW [id]`, `CAL PT <mV/V> <mbar>`, `CAL APPLY` | Enter a table by hand (up to 32 points), then make it active |

A bench cal: pump down, `CAL VAC`; vent, `CAL AIR 1013.2` (your reference reading); check with `CAL?`; then `CAL SAVE`. Either point can be redone on its own.

Valve, control and step test (engineering; the screen does all of this on the cryostat). Nothing moves at power-up; the valve is NOT HOMED until homed:

| Command | What it does |
| --- | --- |
| `VALVE?` | Mode, count and % open, settings, latched faults, target and regulation state, any waiting switch question |
| `VALVE CAL` | Home: off the switch if pressed, fast close to the switch, back off, slow close, back off again; that last point is 0 % |
| `VALVE CLOSED` | Take the valve as seated where it stands (the seat count, the return below 0 %), without the switch: only when you know it is seated (Home valve > Set position > Closed on the screen) |
| `VALVE SETZERO` / `VALVE SKIP` | After "closed switch pressed" (W-1) during a move: call this point 0 %, or keep the count |
| `VALVE SAVED?` | The last saved valve position, whether it was saved at standstill or while moving, and whether it is on offer |
| `VALVE RESTORE` | Take the saved position as the count (at start-up only, before anything moves). The screen asks the same: "Home / Set position / Use X %". It is never used without the operator |
| `VALVE POSMAX\|FHOME\|FCREEP\|BACKOFF\|RETURN\|CLEAR\|OFFSET <n>` | Settings: POS_MAX (66048, 4 turns), homing speeds (400, 25 steps/s), back-off (400, before the slow close), return (400, after it, 0.13.6), clearance (800), count at 0 % (0). `CFG SAVE` keeps them |
| `CTRL?` | Latched faults, highest priority first |
| `PID?` | The control parameters (KP, TI, OUTMIN, DB, KDIR and BAND can be set; the rest are fixed in `ctrl.h`) |
| `PID KP\|TI\|OUTMIN\|DB\|BAND <v>` | Gain (%/mbar, 4.00), integral time (min, 0.25; 0 off), AUTO min (%, 0.0), deadband (steps, 8), target band (mbar, 0.10). Live, AUTO included; `CFG SAVE` keeps them |
| `PID KDIR +1\|-1` | Pressure response: +1 when opening the valve raises the pressure |
| `TUNE STEP [<+-%>]` | Step test for KP and TI, in MANUAL with the valve homed: takes up the drive's slack (5 % the other way and back), waits for a steady pressure, steps the valve (default +1.0 % of travel, doubled until the pressure moves 0.2 mbar, up to 20 %), steps again the same way, records each step for 60 s or until settled, fits dead time and time constant, and suggests KP and TI by the Lambda rule. Applies nothing; the valve returns to where it started. Any other valve command, STOP, AUTO or a fault ends it (`InitialTesting/tune.h`) |
| `TUNE?` \| `TUNE STOP` | Progress and the result / end the test, valve back to the start |

Target, AUTO and MANUAL, valve moves, STOP, the alarm reset and OK and the event log are the remote set's commands in a bench build too (`PRES SP`, `MODE`, `VALV SET`, `STOP`, `ALRM`, `EVNT?`).

Faults latch until `ALRM RESET` (or Reset on the screen): F-1 no pressure signal, F-2 excitation outside 9.5 to 10.5 V, F-3 switch not found while homing, F-4 switch still pressed after moving off it, F-5 motion timeout, F-6 no pressure response (AUTO only). F-3 to F-6 stop the motor and refuse moves; the sensor faults do not, so the valve can be moved and homed with no Druck connected. F-1 and F-2 only reset once the cause has gone. In AUTO, moves, homing and the valve settings answer "Switch to MANUAL first".

Valve motor (Stepper 3, engineering; refused while homing or with a fault latched, except `MOT STOP` and `MOT OFF`):

| Command | What it does |
| --- | --- |
| `MOT?` | Position, rates, mode, coils, limit |
| `MOT MOVE <steps>` | Move relative, 1 to 700000 steps; positive opens, negative closes. Closing is refused while the limit reads open. During a move, relative to where that move ends |
| `MOT GOTO <pos> [sps]` | Move to an absolute position, at its own rate if given (16 to 1000). During a move it retargets: a target further on extends the move, one behind ramps down and turns round |
| `MOT STOP` | Stop at once (coils stay on if hold is on) |
| `MOT STOP RAMP` | Ramp down at the normal deceleration, then stop |
| `MOT OFF` | Stop and de-energise the coils. In AUTO, `MOT STOP`, `MOT STOP RAMP` and `MOT OFF` end AUTO first (MANUAL, as `STOP`) |
| `MOT ZERO` | Call the current position 0 |
| `MOT OVERRIDE` | Let the next move close past an open limit switch (one move, not saved); `MOT OVERRIDE OFF` cancels |
| `MOT RATE <16-1000>` | Run rate, steps/s |
| `MOT START <16-1000>` | Start rate, steps/s |
| `MOT ACCEL <10-20000>` | Acceleration, steps/s² |
| `MOT MODE WAVE\|FULL\|HALF` | Step sequence; the count is rescaled and the homing kept (valve settings are in full steps) |
| `MOT HOLD 0\|1` | Keep the coils on when stopped |
| `MOT DIR 0\|1` | Reverse the direction (0, normal, by default) |
| `MOT ORDER <abcd>` | Coil order, digits 0 to 3 once each, e.g. `MOT ORDER 0123` |

`CFG SAVE` keeps the motor settings (`RATE` to `ORDER`).

The settings commands (`RATE` to `ORDER`, and `ZERO`) answer `ERR Busy, MOT STOP first` during a move.

## Tools

- `tools/logger/log_druck.py <IP or COM>` polls the pressure, target, valve, mode, alarms and room pressure once a second into a CSV and reconnects on its own.
- `tools/cal/druck_cal.py <IP or COM> show|zs|load|erase` manages the Druck calibration table in serial flash (a bench build only, 0.13.0 on).
- `tools/ui-preview/build.sh` renders the screens (Home, Menu, keypad, dialogs, the stage 5 and 6 pages) to PNG on a PC (`out/new-*.png`), with the real settings store over a flash held in RAM, and checks the LVGL heap returns after opening and leaving screens.
- `tools/ctrl-test/run.sh` runs `ctrl.c` on a PC against a C port of the simulator's flow line (helium pot, needle valve, pump, the motor ramp and the closed switch) and checks it against the simulator's own results: closed to 5 mbar, 5 to 4.5 and 4.5 to 10 mbar, targets 5, 7, 3 and 5 with no false trip, helium off (F-6), pump off (W-2 to MANUAL), a slipping drive (F-6) and the entry refusals.
- `tools/cfg-test/run.sh` tests the settings store and the saved valve position on a PC against a flash held in RAM (version 1 record loading, saves, Previous save, refused combinations, sensors, sensor calibration, the position log wrapping).
- `tools/xbuild/build.sh [Release|Debug]` cross-builds the firmware on Linux (cmake, ninja, Ubuntu `gcc-arm-none-eabi` and `libnewlib-arm-none-eabi`) with the mikroSDK libraries in `.meproject` and prints flash and RAM use. For size checks only; the flashed build is NECTO's.

## Related documents

- `simulator/README.md`: the simulator (the firmware in a browser), how to rebuild it, and the history of the JavaScript simulator the screens were designed in.
- Project documents (outside the repository): Control Logic, MM4 Design Description, Firmware Porting Plan, Firmware Guide, and the bench-test notes for each stage (`notes/stageN-bench-test.md` in the project files).
