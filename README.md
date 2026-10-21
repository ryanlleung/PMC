# PMC

Pressure-based needle-valve controller for helium-flow cryostats, developed during a 12-week graduate placement in a sample environment group.

## Context

Oxford Instruments cryostats (CF-10, CF-11, HM-2, AVM-1, AVM-2) draw helium through a needle valve, driven by a geared stepper motor and regulated today by a Mercury iTC. The iTC holds the pressure measured by a Druck transducer on the pumping line. Pressure is a rough proxy for the helium flow; the flow itself is not measured. This project replaces the iTC's valve control with a standalone controller that keeps the existing Druck transducer and stepper valve.

How it is used: operators set a target pressure for each cryostat from experience (typically 1 to 25 mbar) and switch between MANUAL and AUTO. They do not tune the control loop. They watch the sample temperature on another controller, so the pressure trend on this one matters.

Constraints: bought modules only (no PCB design), no PLC, Modbus TCP and a small touchscreen, 0.1 mbar resolution. The valve's limit switch is the closing stop and is read in firmware; there is no hardware interlock.

Two hardware options are being built and compared; neither is final:

- **Option C (this repository):** a MikroE Mikromedia 4 (STM32F407) touchscreen board with Click boards for the stepper driver, the Druck excitation and the Druck reading. All control and the screens are in this firmware.
- **Option A:** a Eurotherm EPC3008 controller with an Applied Motion STF06-R stepper drive in Modbus position mode. The EPC runs the PID and writes the valve target to the drive over RS-485. Not in this repository.

`simulator/` holds the screen simulator (HTML) the firmware screens are ported from.

## This repository

This repository holds the Option C prototype: firmware for the MikroE Mikromedia 4 (STM32F407ZG) with Click boards, built in NECTO Studio 7 (mikroSDK, LVGL 9.4). The firmware is in `InitialTesting/`.

Firmware version: `PMC_FW_VERSION` in `InitialTesting/pmc_config.h` (0.10.11 at the time of writing). Commit titles start with the version they build.

## Status

The simulator is being ported to the firmware in stages, one version and one pull request each, each passed on the bench before the next:

| Stage | Version | Content | State |
| --- | --- | --- | --- |
| 0 | 0.3.x | Baseline: Druck reading, bench calibration, limit switch | Merged (#10) |
| 1 | 0.4.x | Motion: absolute moves with retarget, ramped and immediate stop, F-13 timeout | Merged |
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
| mikroBUS shield, socket S1: Stepper 3 Click (ULN2003) | Drives the unipolar six-lead valve stepper (PK22, 344:1 gearbox), 12 V | AN PA4, RST PC2, CS PB12, PWM PD12 |
| Socket S2: limit switch (Adapter Click and Pressure 21 Click on order) | One NC dry contact to GND, opens at the closed end of travel | PWM pad PD13 (internal pull-up), GND |
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
2. Once a second: `sense_second()` reads the INA228, converts to pressure, updates the faults and the trend, and sends the DATA and status lines.
3. One command line from the PC, if any, offered to each module's `*_command()` in turn.
4. `lv_timer_handler()` (screens refresh at 2 Hz, the fast parts at 10 Hz), the watchdog kick, 1 ms delay.

Steps are timed by TIM7 interrupts, so the motor runs smoothly while the screen draws.

| File | What it does |
| --- | --- |
| `pmc_config.h` | Build options: link (USB or Ethernet), version, Stepper 3 socket, limit input pin and polarity |
| `clicks.c` | Boost 10 excitation set-up and 11.5 V trip, INA228 reads, Druck plausibility, pressure from the cal |
| `cal.c` | Druck calibration table: ratio (mV/V) against pressure, linear between points; bench two-point (CAL VAC, CAL AIR) |
| `uncert.c` | Pressure uncertainty budget (`UNC?`), the Druck term from the full scale of the sensor in use |
| `sense.c` | The once-a-second read, DATA and status lines |
| `limit.c` | Limit switch input: a trip counts at once, the release is debounced (20 ms) |
| `stepper3.c` | Step sequences (wave, full, half), ramps, absolute moves with retarget, the limit stop, F-13 timeout |
| `valve.c` | Modes (NOT HOMED, HOMING, MANUAL, AUTO, AUTO PAUSED), homing, % position, STOP, fault latches, the event log (200 kept), runs AUTO |
| `ctrl.c` | AUTO control step, ported from the simulator: PI, anti-windup, deadband, response monitor, F-02 and W-02 checks (no hardware calls; `tools/ctrl-test` runs it on a PC) |
| `valvepos.c` | Saves the step count at standstill so a restart can offer it |
| `cfg.c` | Settings record in serial flash, two sectors in turn, CRC32, read back before it counts |
| `trend.c` | Trend rings in RAM (11.5 KB): 600 one-second samples and 360 ten-second means, on fixed one-second slots; slots the loop missed go in as no-data samples. `TREND DUMP` is sent a line at a time as the link has room |
| `extflash.c` | SST26 serial flash driver |
| `rtclock.c`, `sysinfo.c` | Clock (`TIME?`, `TIME SET`); version and reset cause (`VER?`) |
| `link.c`, `usb_serial.c`, `ethlink.c`, `eth_hw.c` | PC text link over USB CDC or TCP |
| `ui.c` | Screen framework: header, status strip, STOP, buttons, cards, keypad, pick lists, dialogs, text view |
| `ui_home.c` | Home and Menu |
| `ui_valve.c` | Valve, Home valve, Faults and log, Diagnostics |
| `ui_settings.c` | Settings pages and Calibrate sensor |
| `ui_trend.c` | Trend page and the Home sparkline (drawn on a draw event, no lv_chart) |
| `motor_screen.c` | The 0.6.0 engineering motor screen, opened from Settings > Motor override; built on open and freed on leave |
| `driver/` | Display and touch ports for LVGL; `display_px.c` is the pixel loop |
| `fixed.c` | Fixed-point text and parsing (lv_snprintf has no float) |

## Operation

- **Nothing moves at power-up.** The valve is NOT HOMED until it is homed, or until the operator accepts the position saved before the restart ("Use saved X % / Home"). The saved position is never used on its own.
- **Homing** (Home valve, or `VALVE CAL`): if the switch is pressed, move off it first (F-09 if it stays pressed); 1 fast close to the switch (F-07 if not found within 1.1 x full travel); 2 back off; 3 slow close onto the switch. The switch point is 0 %. STOP cancels it and keeps the earlier homing.
- **MANUAL**: on the Valve page, tap the Go to box to type a %; hold Close or Open; 1 % and 5 % steps; Fully close and Fully open. In AUTO the Go to box reads auto and a tap asks to switch to MANUAL. Position is in % of POS_MAX steps (66048, 4 turns, by default).
- **Limit switch** is the closing stop, read in firmware before every closing step; there is no hardware interlock (decided). A trip during a normal move stops it and asks "Set as 0 % / Skip" (W-01); the count is never zeroed on its own. Limit override lets one move close past an open switch.
- **Motor settings**: changing the step mode, direction or coil order, or zeroing the motor count (`MOT ZERO`, the motor screen, a loaded record), clears the homing; the valve must be homed again.
- **Saved position**: written 2 s after the valve stops, and withdrawn (read back and checked, one retry) before the next move starts, so a power cut mid-move leaves nothing to offer. If it cannot be withdrawn the move is refused. At start-up every slot of the log is read, so an entry after a failed write is still found.
- **AUTO** (Start AUTO, or `CTRL AUTO`): needs the valve homed, no fault, a valid reading and a target. The first Start AUTO asks for the target and Set starts AUTO; later ones confirm the target kept or offer New target. The target changes from the Target box (over 5 mbar asks first). Once a second, after the reading, `ctrl.c` works out the valve command: PI in position form (D on the measurement, off), bumpless entry from where the valve is going, no integration past 100 % or Auto min, while the switch blocks closing, or while the valve is more than 3000 steps short of the last command, and commands inside the deadband held. The status line reads Regulating to X, In band (within the band) and Stable (60 s in the band). Switch to MANUAL ramps the valve down and holds it where it stops.
- **AUTO checks**: F-02 pressure above the sensor range for 5 s; a response monitor over 30 s windows (the valve position through a 40 s lag, moves under 1000 steps not judged): F-10 after 5 windows with no pressure response, F-16 opening while the pressure falls and F-17 closing while it rises, two windows in a row. W-02 after 1800 s outside the band stops the valve and pauses AUTO (Resume AUTO or Switch to MANUAL). Any fault in AUTO stops the valve and drops to MANUAL, holding where it stopped.
- **STOP** outside AUTO stops and holds the valve with no latch. In AUTO it latches F-12 and drops to MANUAL.
- **Faults** latch until Reset fault (or `CTRL RESET`), which clears only those whose cause has gone: F-01 no Druck signal, F-03 excitation outside 9.5 to 10.5 V, F-07 switch not found while homing, F-09 switch still pressed after moving off it, F-10, F-16, F-17 and F-02 (AUTO, above), F-13 motion timeout, F-12 STOP in AUTO. F-07, F-09, F-10, F-16, F-17, F-13 and F-12 block moves until reset; F-01, F-03 and F-02 do not.

## Screens

480x272, after the simulator (`simulator/`, ISA-101 style: grey for normal, colour only for abnormal states). Every screen has a header (Back and Home buttons, the title centred, the mode word on the right with its dot: a grey ring, filled grey in AUTO, red with a fault; a red rule under the header with a fault), a status strip and STOP on the bottom row. Under a popup the strip stays, updating and greyed. On Home and the operator screens the strip shows the status line (the mode dot and Valve at X %, Moving to X %, or in AUTO Regulating to / In band / Stable at the target) with the 5 min pressure sparkline on its right; prompts, faults and messages take the whole strip. Sparklines and Trend draw each time slot through its first sample, the two either side of its largest change and its last, so a step shows upright.

- **Home** (title "<cryostat name> pressure controller", for example "CF-11 pressure controller", on the left): Pressure tile (40 px reading in mbar, log bar labelled 0.1, 1, 10, 100 and 1k with the end lines the same distance in from each end, Target box) and Valve tile (40 px reading in %, bar, Go to box), the mode button (Home valve / Start AUTO / Switch to MANUAL / Resume AUTO), Valve, Menu, the pressure card (a tap on the sparkline opens Trend).
- **Menu**: Records (Trend, Faults and log), Calibrate (Valve homing, Sensor), System (Diagnostics, Settings).
- **Valve**: manual moves and the valve's state. **Home valve**: homing state, motor step count, limit switch, the three steps and the time taken. **Faults and log**: latched faults (tap for detail and help) and the event log. **Diagnostics**: readings and hardware state.
- **Trend**: pressure (left axis), the target while in AUTO (dashed), and valve (right axis, 0 to 100 %) over 1, 2, 5, 10, 30 min or 1 hour. Each point is the mean of a fixed slot of time counted from start-up, so the line keeps its shape as it scrolls.
- **Settings**: Cryostat name (a keyboard; up to 12 characters, in use at once and unsaved until Save settings), Choose sensor (up to four Druck sensors, each with its own cal; one in use), Valve setup (travel, direction; homing speeds and back-off; drive rates), Control tuning (for AUTO), Review and save, Load settings (last save, previous save, factory), Limit override (while armed the strip says so on every screen, with Cancel), Motor override (the 0.6.0 motor screen; its cross closes it). Edits are drafts until Save settings, which checks them together and saves all or nothing.
- **Calibrate sensor**: the bench two-point cal from the screen (vacuum, then air at a barometer reading).

Display formats: pressure 1 decimal, valve % and keypad 2 decimals, dates DD/MM/YY. Fonts are Montserrat Medium 12, 16 and 26 px (`ui_font_*.c`, converted with lv_font_conv, ASCII plus ± ° µ – •), and 40 px digits only for the Home readings (`ui_font_40.c`). The display is driven over the SSD1963's 16-bit 8080 bus, bit-banged on GPIOE (WR on PF11); touch is read over I2C every 5 ms.

LVGL heap: 64 KB in CCM plus a 24 KB second pool in main RAM. `PERF?` reports loop and draw times and heap use.

## Pressure measurement

The Druck output is ratiometric, so the firmware uses the ratio of signal to excitation (mV/V), read by the INA228 with 128 averages over 600 ms (30 cycles of 50 Hz mains, `PMC_PM_MAINS_AVG`), once a second. `cal.c` turns the ratio into pressure. The normal cal is the bench two-point: a pumped-down vacuum point (`CAL VAC`, or Calibrate sensor) and an air point at a barometer reading (`CAL AIR`). Both are read live through the INA228 as wired, so the INA228's loading of the Druck output (the reading is about 5 % low uncalibrated) and its gain and offset sit inside the cal. With no cal saved, the nominal 0 to 100 mV at 10 V is used and the screen says so. The display shows 0.1 mbar; accuracy is set by the Druck (±0.04 % FS, ±0.41 mbar) and the cal points (`UNC?`).

## Storage

Serial flash (survives reflashing; neither the application nor mikroBootloader erases it):

| Address | Holds |
| --- | --- |
| 0x7EC000 to 0x7FBFFF | Saved valve position, a log of 16-byte entries over sixteen 4 KB sectors (one erase per 256 entries) |
| 0x7FD000 and 0x7FE000 | Settings record (version 3), written to the two sectors in turn |
| 0x7FF000 | Old stand-alone cal record (0.3.30 to 0.5.x), read only if no settings record exists |

The trend and the event log are in RAM and empty after a restart.

## Build and flash

- `InitialTesting/pmc_config.h` sets the PC link: `PMC_LINK_ETHERNET 1` gives TCP port 5000 (DHCP, or 169.254.10.50 if no DHCP server answers), and `0` gives the USB COM port. The two can't run together, because RMII needs 50 MHz from MCO1 and USB needs 48 MHz from the same PLL.
- The linker script `.meproject/setup/FileLinker/stm32f407zg.ld` is patched so `.ccmram` is `(NOLOAD)`, which keeps the LVGL heap that lives there out of the flash image. If NECTO ever regenerates this file, re-apply the patch.
- Build Debug in NECTO. Release leaves the display blank: MikroE's prebuilt Release `lib_ssd1963.a` was compiled with a non-volatile GPIO register struct, so `ssd1963_write_command` loses its WR low and the SSD1963 never latches a command (bench, 0.3.33; disassembly, 0.8.9). From 0.8.10 (pixel loop from 0.8.11) the Debug build keeps the Debug SDK libraries and the hardware drivers at -O0 but compiles LVGL, the pixel loop `driver/display_px.c` and `ui*.c` at -O2 (`driver/display_lvgl.c` must stay -O0: it inlines the SDK's WR_LOW/WR_HIGH) (`InitialTesting/CMakeLists.txt`), so screens draw faster. About 630 KB of flash (0.10.4).
- NECTO's build folders (`build-MM4/`, `build-Default Configuration/`, ...), `compile_commands.json` and the clangd index in `.cache/` are not tracked, so a NECTO build no longer blocks `git pull`. The one exception is `build-MM4/generated/` (screen code from `main_screen.mscr`), which `tools/ui-preview` compiles. NECTO also rewrites `.meproject/config.json` (it holds machine paths, but also the board and MCU selection, so it stays tracked): before a pull, `git stash` or `git checkout -- InitialTesting/.meproject/config.json`.
- After `CMakeLists.txt` changes (new files), Clean before building in NECTO.
- Flash over USB-C with mikroBootloader (no debugger). Check `VER?` after flashing.

## PC link commands

Connect to the USB COM port (any baud rate, it is virtual) or, in the Ethernet build, TCP port 5000 (e.g. PuTTY in Raw mode). Send one command per line; CR, LF or CRLF all end a line. Commands are upper case and exact. The board echoes each line it receives as `> <line>`. Spaces at either end are ignored. Each reply ends with `OK`, or is one `ERR ...` line saying what was expected. Type `HELP` (or `?`) on the board for the groups, then `HELP SYS`, `HELP CFG`, `HELP CAL`, `HELP VALVE` or `HELP MOT`.

Unprompted output: a `DATA,t_ms,p_mbar,signal_uV,exc_mV,ratio_mV_per_V,die_C,set_mV,ok` line once a second (after `DATA ON`; off at boot), status lines when they change, and `LIM closed` / `LIM OPEN` when the limit switch changes, and `F-13 motion timeout: ...` if a move runs longer than its distance / rate plus the ramps plus 2 s (the motor is stopped at once).

| Command | What it does |
| --- | --- |
| `HELP` or `?` | List the command groups; `HELP SYS`, `HELP CFG`, `HELP CAL`, `HELP VALVE`, `HELP MOT` list one group each |
| `VER?` | Firmware version and build time |
| `DATA OFF` / `DATA ON` | Pause or resume the DATA and status lines; off at boot (the logger sends DATA ON) |
| `TIME?` | Read the real-time clock |
| `TIME SET YYYY-MM-DD HH:MM:SS` | Set the clock, e.g. `TIME SET 2026-10-13 14:30:00` |
| `LIM?` | Valve limit switch: state, raw pin level, change count |
| `UNC?` | Pressure uncertainty budget, term by term |
| `TOUCH?` | Touch controller read and error counters |
| `PERF?` | Longest main loop and display timings since the last `PERF?`, and the LVGL heap (total, used, peak since boot, biggest free block, fragmentation) |
| `PM DIAG` | Steps the excitation from 9 to 11 V, one step a second, then puts it back (only when `PMC_PM_DIAG` is 1) |

Settings store (motor, valve, control tuning, up to four pressure sensors each with its own cal, the cryostat name and the save time, in one record in serial flash; version 3 from 0.10.9, a version 2 record (0.9.0 to 0.10.8) loads with the name CF-11, and a 0.6.0 to 0.8.x record still loads as one 15 psi sensor). Changes over the link are live at once and unsaved until `CFG SAVE`. At power-up the newest valid record is loaded; with none, the factory defaults (and a cal saved by firmware 0.3.30 to 0.5.x, if any):

| Command | What it does |
| --- | --- |
| `CFG?` | Both flash sectors (revision, valid or why not), what was loaded at power-up, number of unsaved settings |
| `CFG DIFF?` | Each setting that differs from the newest save |
| `CFG SAVE` | Save everything. Two sectors are used in turn; a save is read back before it counts, so a failed save keeps the last good record |
| `CFG LOAD LAST` / `CFG LOAD PREVIOUS` | Load the newest / the other saved record (shows as unsaved until saved). Refused while moving or homing |
| `CFG LOAD DEFAULTS` | Factory motor, valve and tuning settings; the sensors and their cals are kept |
| `CFG NAME <text>` / `CFG NAME?` | Set / show the cryostat name in the Home title (up to 12 characters; unsaved until `CFG SAVE`) |
| `CFG TEST CORRUPT` / `CFG TEST ERASE` | Bench tests: spoil the newest record's CRC / erase both sectors |

Calibration (Druck, through the INA228):

| Command | What it does |
| --- | --- |
| `CAL?` | Active cal, whether it is saved, and the bench points held |
| `CAL VAC [mbar] [unc]` | Capture the vacuum point over 10 readings. Defaults: 0 mbar, ±0.010 mbar. Range 0 to 50 mbar |
| `CAL AIR <mbar> [unc]` | Capture the air point at the reference reading, 500 to 1100 mbar. Default ±1.0 mbar |
| `CAL SAVE` | Same as `CFG SAVE`: saves all settings, the active cal included (it is only in RAM until then) |
| `CAL ERASE` | Go back to nominal, erase the old 0.5.x cal record and save |
| `CAL NEW [id]`, `CAL PT <mV/V> <mbar>`, `CAL APPLY` | Enter a table by hand (up to 32 points), then make it active |

A bench cal: pump down, `CAL VAC`; vent, `CAL AIR 1013.2` (your reference reading); check with `CAL?`; then `CAL SAVE`. Either point can be redone on its own.

Valve (homing, position, STOP, faults). Nothing moves at power-up; the valve is NOT HOMED until `VALVE CAL` (homing):

| Command | What it does |
| --- | --- |
| `VALVE?` | Mode, count and % open, settings, latched faults, any waiting switch question |
| `VALVE CAL` | Home: off the switch if pressed, fast close to the switch, back off, slow close; the switch point is 0 % |
| `VALVE GOTO <0-100>` | Go to % open (one decimal), once homed |
| `VALVE CLOSED` | Take the valve as fully closed (0 %, homed) where it stands, without the switch: only when you know it is seated (Home valve > Set as closed on the screen) |
| `VALVE SETZERO` / `VALVE SKIP` | After "closed switch pressed" (W-01) during a move: call this point 0 %, or keep the count |
| `VALVE SAVED?` | The valve position saved at the last standstill, and whether it is on offer |
| `VALVE RESTORE` | Take the saved position as the count (at start-up only, before anything moves). The screen asks the same: "Use saved X % / Home". It is never used without the operator |
| `VALVE POSMAX\|FHOME\|FCREEP\|BACKOFF\|CLEAR\|OFFSET <n>` | Settings: POS_MAX (66048, 4 turns), homing speeds (400, 25 steps/s), back-off (400), clearance (800), switch count (0). `CFG SAVE` keeps them |
| `CTRL SP <mbar>` | Target, kept to 0.1 mbar, 0 to the sensor range (not saved: none at start-up) |
| `CTRL AUTO` | Start or resume AUTO after the entry checks. While in AUTO one line a second reads `AUTO 4.62 mB, target 4.8, valve 14.20 % to 14.35 %, In band`, in place of the motor status line |
| `CTRL MAN [<0-100>]` | To MANUAL: the valve ramps down and holds where it stops, then goes to % if given |
| `PID?` | The control parameters: KP, TI, OUTMIN, DB, KDIR and BAND can be set, the rest are fixed in `ctrl.h` |
| `PID KP\|TI\|OUTMIN\|DB\|BAND <v>` | Gain (%/mbar, 4.00), integral time (min, 0.25; 0 off), Auto min (%, 14.0), deadband (steps, 8), target band (mbar, 0.10). Live, AUTO included; `CFG SAVE` keeps them |
| `PID KDIR +1\|-1` | Pressure response: +1 when opening the valve raises the pressure |
| `CTRL STOP` | Stop now; in AUTO it latches F-12. Cancels homing |
| `CTRL RESET` | Clear the latched faults whose cause has gone |
| `CTRL?` | Latched faults, highest priority first |
| `EVT? [1-200]` | The last events (default 20), sent a line at a time as the link has room. Each event is also sent as it happens: `EVT <s> <tag> <text>` |

Faults latch until `CTRL RESET`: F-01 no Druck signal, F-03 excitation outside 9.5 to 10.5 V, F-07 switch not found while homing, F-09 switch still pressed after moving off it, F-10 no pressure response, F-16 and F-17 pressure moving the wrong way, F-02 above the sensor range (AUTO only), F-13 motion timeout, F-12 STOP in AUTO. All but F-01, F-03 and F-02 stop the motor and refuse VALVE and MOT moves; from 0.9.10 the sensor faults do not, so the valve can be moved and homed with no Druck connected. F-01, F-03 and F-02 only reset once the cause has gone. In AUTO, VALVE and MOT moves, homing and the valve settings answer "Switch to MANUAL first" (`CTRL MAN`).

Valve motor (Stepper 3, engineering; refused while homing or with a fault latched, except `MOT STOP` and `MOT OFF`):

| Command | What it does |
| --- | --- |
| `MOT?` | Position, rates, mode, coils, limit |
| `MOT MOVE <steps>` | Move relative, 1 to 400000 steps; positive opens, negative closes. Closing is refused while the limit reads open. During a move, relative to where that move ends |
| `MOT GOTO <pos> [sps]` | Move to an absolute position, at its own rate if given (16 to 1000). During a move it retargets: a target further on extends the move, one behind ramps down and turns round |
| `MOT STOP` | Stop at once (coils stay on if hold is on) |
| `MOT STOP RAMP` | Ramp down at the normal deceleration, then stop |
| `MOT OFF` | Stop and de-energise the coils |
| `MOT ZERO` | Call the current position 0 |
| `MOT OVERRIDE` | Let the next move close past an open limit switch (one move, not saved); `MOT OVERRIDE OFF` cancels |
| `MOT RATE <16-1000>` | Run rate, steps/s |
| `MOT START <16-1000>` | Start rate, steps/s |
| `MOT ACCEL <10-20000>` | Acceleration, steps/s² |
| `MOT MODE WAVE\|FULL\|HALF` | Step sequence |
| `MOT HOLD 0\|1` | Keep the coils on when stopped |
| `MOT DIR 0\|1` | Reverse the direction (1, reversed, by default) |
| `MOT ORDER <abcd>` | Coil order, digits 0 to 3 once each, e.g. `MOT ORDER 0123` |

`CFG SAVE` keeps the motor settings (`RATE` to `ORDER`).

The settings commands (`RATE` to `ORDER`, and `ZERO`) answer `ERR busy, MOT STOP first` during a move.

## Tools

- `tools/logger/log_druck.py <IP or COM>` logs the once-a-second DATA lines to CSV and reconnects on its own.
- `tools/cal/druck_cal.py <IP or COM> show|zs|load|erase` manages the Druck calibration table in serial flash.
- `tools/ui-preview/build.sh` renders the screens (Home, Menu, keypad, dialogs, the stage 5 and 6 pages) to PNG on a PC (`out/new-*.png`), with the real settings store over a flash held in RAM, and checks the LVGL heap returns after opening and leaving screens.
- `tools/ctrl-test/run.sh` runs `ctrl.c` on a PC against a C port of the simulator's flow line (helium pot, needle valve, pump, the motor ramp and the closed switch) and checks it against the simulator's own results: closed to 5 mbar, 5 to 4.5 and 4.5 to 10 mbar, targets 5, 7, 3 and 5 with no false trip, helium off (F-16), pump off (F-17), a slipping drive (F-10) and the entry refusals.
- `tools/cfg-test/run.sh` tests the settings store and the saved valve position on a PC against a flash held in RAM (version 1 record loading, saves, Previous save, refused combinations, sensors, sensor calibration, the position log wrapping).
- `tools/xbuild/build.sh [Release|Debug]` cross-builds the firmware on Linux (cmake, ninja, Ubuntu `gcc-arm-none-eabi` and `libnewlib-arm-none-eabi`) with the mikroSDK libraries in `.meproject` and prints flash and RAM use. For size checks only; the flashed build is NECTO's.

## Related documents

- `simulator/README.md`: the screen simulator and its history.
- Project documents (outside the repository): Control Logic, MM4 Design Description, Firmware Porting Plan, Firmware Guide, and the bench-test notes for each stage (`notes/stageN-bench-test.md` in the project files).
