# PMC

Pressure-based needle-valve controller for helium-flow cryostats: the MercShore iPC.

Oxford Instruments cryostats (CF-10, CF-11, HM-2, AVM-1, AVM-2) draw helium through a needle valve driven by a geared stepper motor. The controller holds the pressure measured by a Druck transducer on the pumping line by moving that valve. Pressure is a rough proxy for the helium flow; the flow itself is not measured. The controller keeps the cryostat's existing Druck transducer and stepper valve.

Operators set a target pressure for each cryostat (typically 1 to 25 mbar) and switch between MANUAL and AUTO. They do not tune the control loop. The pressure trend is shown because it is what they watch.

The controller is a MikroE Mikromedia 4 (STM32F407ZG) touchscreen board with four Click boards (see Hardware), built from bought modules only. It has a 4.3 in touchscreen, a text command port and Modbus TCP over Ethernet, and shows pressure to 0.1 mbar. The valve's limit switch is the closing stop and is read in firmware; there is no hardware interlock.

## Repository

| Folder | Holds |
| --- | --- |
| `InitialTesting/` | The firmware (NECTO Studio 7 project, mikroSDK, LVGL 9.4) |
| `docs/` | Operation manual (`mm4-operation-manual.md`), Modbus register map (`modbus-register-map.md`), hardware schematics and wiring (`hardware/`), HMI reference |
| `simulator/` | The firmware compiled to WebAssembly, run in a browser over a simulated bench (`simulator/README.md`) |
| `secop/` | A reference Frappy (SECoP) driver for the text port (`secop/README.md`) |
| `tools/` | PC tests, screen previews, a cross-build, a logger and calibration helpers |

The firmware version is `PMC_FW_VERSION` in `InitialTesting/pmc_config.h`; `*IDN?` reports it.

## Hardware

The controller is one MikroE development board with a socket shield and four Click boards. MikroE calls its small add-on modules "Click boards"; each plugs into a mikroBUS socket (two 1x8 headers carrying SPI, I2C, UART, PWM, an analog pin, reset, 3.3 V and 5 V). The names below are MikroE's product names. Schematics and wiring diagrams are in [`docs/hardware/`](docs/hardware/).

### Boards

| Board | MikroE part | Main chip | Where | What it does in the controller |
| --- | --- | --- | --- | --- |
| Mikromedia 4 for STM32 Capacitive | MIKROE-3618 | STM32F407ZG (168 MHz, 1 MB flash, 192 KB RAM); SSD1963 display controller; LAN8720A Ethernet PHY; SST26VF064B 8 MB serial flash | Main board | Runs the firmware. 4.3 in 480x272 capacitive touchscreen, 12 V terminal, USB-C (power and flashing), real-time clock |
| Mikromedia 4 Capacitive Shield | MIKROE-3631 | none | Plugs onto the back of the main board | Four mikroBUS sockets, S1 to S4, and the RJ45 for Ethernet |
| Adapter Click | MIKROE-1432 | none (a pass-through) | Socket S1 | Raises the Pressure 21 Click above the shield and brings every socket pin out on a 2x5 IDC10 header. The valve limit switch is wired to this header |
| Pressure 21 Click | Pressure 21 Click | Bosch BMP581 barometric sensor | On top of the Adapter Click in S1 | Room (atmospheric) pressure. Used as the air point of the transducer calibration and shown on Diagnostics and `BAROMETER?` |
| Stepper 3 Click | MIKROE-2035 | ULN2003 Darlington array | Socket S2 | Drives the valve's unipolar stepper motor from the 12 V motor supply |
| Boost 10 Click | MIKROE-6927 | LT8337 boost converter, TPL0501 digital potentiometer | Socket S3 | Makes the 10 V excitation for the Druck transducer from the socket's 5 V |
| Power Monitor Click | MIKROE-4810 | TI INA228 | Socket S4 | Measures the Druck output (shunt input, ±163.84 mV) and the excitation (bus input), so the firmware can use their ratio. Its die temperature drives the W-3 warning |

Off-board parts: the Druck UNIK 5000 transducer (PDCR50G1, 15 psia absolute, 10 mV/V, M12 4-pin), the existing Oxford Instruments needle valve (Portescap 26M048B2U or 35M048B2U unipolar stepper, PK22 344:1 gearbox, NC limit switch, Fischer 7-pin), and a PoE splitter giving regulated 12 V.

### Connections

| From | To | Pins |
| --- | --- | --- |
| Druck M12 pin 1, +Supply | Boost 10 Click VOUT and Power Monitor Click VBUS | |
| Druck M12 pin 2, +Out | Power Monitor Click IN+ | |
| Druck M12 pin 4, -Out | Power Monitor Click IN- | |
| Druck M12 pin 3, -Supply | Common 0 V | |
| Valve Fischer pins 1 to 4, coils | Stepper 3 Click outputs A to D | Coil order is set on the screen (Motor settings > Override > Motor override > Coils) |
| Valve Fischer pin 5, coil centre taps | Stepper 3 Click COM, the 12 V motor supply | Stepper 3 Click J1 set to external supply |
| Valve Fischer pins 6 and 7, limit switch | Adapter Click IDC10 pad 8 and pad 10 (GND) | Pad 8 is printed RX but is the socket's TX pin, PD5, read with an internal pull-up. The Adapter Click labels its UART pads from the plugged-in Click's side |
| PoE splitter 12 V | Mikromedia 4 12 V terminal and Stepper 3 Click COM | Regulated 12 V. The Stepper 3 Click has no current limit |
| PoE splitter data | Shield RJ45 | Ethernet |
| Board 0 V | Chassis earth | One bond at one point. Without it the floating board picks up noise on the Druck reading |

Socket pins used by the firmware:

| Socket | Board | Signals |
| --- | --- | --- |
| S1 | Adapter Click + Pressure 21 Click | Pressure 21 Click on I2C2 (PF1 SCL, PF0 SDA), address 0x46. Limit switch on PD5 (socket TX) |
| S2 | Stepper 3 Click | AN PB0, RST PC3, CS PA15, PWM PD13: the four coil outputs |
| S3 | Boost 10 Click | SPI1 (PA5 SCK, PA6 MISO, PB5 MOSI), CS PF8, power good PG2 |
| S4 | Power Monitor Click | I2C2 (PF1, PF0), address 0x4A |

On-board: serial flash on SPI2 (CS PB11, SCK PB13, MISO PB14, MOSI PB15), display on a 16-bit 8080 bus on GPIOE (WR PF11), Ethernet PHY on RMII.

### Settings and cautions

- Every Click logic-level jumper is set to 3.3 V.
- Never connect the Boost 10 Click to 12 V: it takes 5 V from the socket and its input maximum is 10 V.
- Never put the Stepper 3 Click in S4: that socket's RST pin (PC13) cannot drive a coil.
- The board takes 10.6 to 14 V on its terminal and chooses 12 V, then USB-C, then Li-Po. SW1 must be on.

## Firmware structure

`main.c` initialises everything (version and reset cause, clock, LVGL and display, calibration, Clicks, limit switch, valve, settings, saved position, screens, links), shows the OFF screen from the first frame, starts the independent watchdog (about 3 to 4 s), then runs one loop with no RTOS:

1. Links, touch (every 5 ms), clock, limit switch, stepper and valve tasks, saved position.
2. Once a second: `sense_second()` reads the Power Monitor Click and the Pressure 21 Click, converts to pressure, updates the faults, the trend and AUTO.
3. One command line from the text port, if any.
4. `lv_timer_handler()` (screens refresh at 2 Hz, the fast parts at 10 Hz), the watchdog kick, 1 ms delay.

Steps are timed by TIM7 interrupts, so the motor runs smoothly while the screen draws.

| File | What it does |
| --- | --- |
| `pmc_config.h` | Build options: link, write access, limit input polarity, network ports and the default IP address, version |
| `clicks.c` | Boost 10 Click excitation set-up and 11.5 V trip, Power Monitor Click reads, Druck plausibility, pressure from the calibration |
| `baro.c` | Pressure 21 Click room pressure: start-up, reads, plausibility window, retry when lost |
| `cal.c` | Druck calibration table: ratio (mV/V) against pressure, linear between points; two-point calibration (vacuum and air) |
| `sense.c` | The once-a-second read and the controller temperature warning |
| `limit.c` | Limit switch input: a trip counts at once, the release is debounced (20 ms) |
| `stepper3.c` | Step sequences (wave, full, half), ramps, absolute moves with retarget, the limit stop, motion timeout |
| `valve.c` | Modes (NOT HOMED, HOMING, MANUAL, AUTO, SHUTDOWN, OFF), homing, Shut down, % position, STOP, fault latches, the event log (100 kept), runs AUTO |
| `ctrl.c` | AUTO control step: PID with KP on the pressure only (no setpoint kick, 0.16.15), gain schedule (four sets from 0.16.14), anti-windup, deadbands, response monitor and settling checks (no hardware calls; `tools/ctrl-test` runs it on a PC) |
| `valvepos.c` | Saves the step count at standstill and every 2 s while moving, so a restart can offer it |
| `cfg.c` | Settings record in serial flash, two sectors in turn, CRC32, read back before it counts |
| `trend.c` | Trend rings in RAM: 600 one-second samples and 360 ten-second means, on fixed one-second slots |
| `extflash.c` | SST26 serial flash driver |
| `rtclock.c`, `sysinfo.c` | Clock; version and reset cause |
| `link.c`, `usb_serial.c`, `ethlink.c`, `eth_hw.c` | Text port over TCP or USB CDC |
| `remote.c` | The command set on the text port |
| `modbus.c`, `mbmap.c` | Modbus TCP server on port 502: the protocol (FC1, FC3, FC5, FC16) and the register map (`docs/modbus-register-map.md`; `tools/modbus-test` runs both on a PC) |
| `ui.c` | Screen framework: header, status strip, STOP, buttons, cards, keypad, pick lists, dialogs, the OFF, shutting down and turning on screens |
| `ui_home.c` | Home and Menu |
| `ui_valve.c` | Valve position, Events, Diagnostics |
| `ui_settings.c` | Motor settings, PID tuning, System, Review and save and Calibrate sensor |
| `ui_trend.c` | Trend page and the Home sparkline |
| `motor_screen.c` | Motor settings > Override > Motor override |
| `ui_font_*.c`, `ui_img_logo.c` | Fonts and the logo shown when turned on |
| `driver/` | Display and touch ports for LVGL; `display_px.c` is the pixel loop |
| `fixed.c` | Fixed-point text and parsing (lv_snprintf has no float) |

## Operation

`docs/mm4-operation-manual.md` is the operator's description. In short:

- **Nothing moves at power-up.** Every power-up and restart starts OFF (black screen, "Double tap to turn on"); only a double tap on the screen turns it on, so AUTO never resumes by itself after a power cut. The valve is NOT HOMED until it is homed or the operator sets the position. With a saved position the start-up screen asks "Home / Set position / Use X %" (it starts "Switched off without Shut down." after a power-off without Shut down); the saved position is never used on its own, except after Shut down (below). Menu > Calibrate > Valve position is one page with Home valve, Set as 0 %, Set as other % and Use last saved. It shows whether the position was homed, entered by hand or restored from a save.
- **Homing** (Home valve): if the switch is pressed, move off it first; close fast to the switch; back off; close slowly onto the switch (the seat); return off the switch by the Return setting (400 steps by default) with the switch released. That last point is 0 %, so the seat reads about -0.6 % with the defaults. STOP cancels homing and keeps the earlier homing.
- **MANUAL**: tap the Go to box on Home to type a %. Position is in % of the valve travel (66048 steps, 4 turns, by default). All normal moves need the valve homed or its position set. Motor override can move by steps without a position reference. Maintain (in place of the SP box in MANUAL) starts AUTO at the pressure shown, with no question, from where the valve is.
- **Limit switch** is the closing stop, read in firmware before every closing step. A trip during a normal move stops it and asks "Set as seat / Skip" (W-1; the seat reads about -0.6 %, as after homing); the count is never zeroed on its own. On the Motor override screen, Limit override is a two-finger deadman: while it is held, Close pressed with a second finger closes past the switch, and the move stops when either finger lifts.
- **AUTO** (Start AUTO in Home's Pressure tile): needs the valve homed or its position set, no fault and a valid reading. It starts at the pressure shown as the setpoint, with no question; the SP box then sets another. Once a second `ctrl.c` works out the valve command: PID in position form (KD on the pressure's rate of change, 0 by default), bumpless entry, no integration past 100 % or AUTO min, while the switch blocks closing, or while the valve lags far behind the last command; commands inside the deadband are held (opening and closing each have their own, so a needle that sticks opening is moved past its stick in one go), and a reversal is held until the command is 0.2 mbar of error at the set's KP from the valve, at most 0.1 % of travel (0.012 % at KP 0.06), so the motor does not turn round inside the drive's backlash. Gain scheduling, as industrial controllers do it: three PID sets, picked on the lower of the pressure and the setpoint against two boundaries, with hysteresis on the pressure (going down, the setpoint's set takes over as the setpoint changes); Off (the default) uses set 1 only. A change of set re-bases the integral, so the valve does not jump, and is logged. The status line reads Regulating to X, In band, or Stable (60 s in the band). Stop AUTO (in Home's Valve tile, in AUTO) or STOP ramps the valve down and holds it where it stops, in MANUAL.
- **Motor supply** is not measured by the firmware (sensing was removed in 0.14.3). Loss of the motor 12 V does not directly end AUTO or stop the step count; the count can then disagree with the valve. The legacy Modbus supply bit is always 1.
- **STOP** stops and holds the valve with no latch; in AUTO it also drops to MANUAL.
- **Shut down** (the power icon at the top left of Home): ends AUTO, moves the valve to 0 % at the run rate (not onto the limit switch), switches the coils off and turns the controller OFF. The question has a "Close the valve to 0 %" checkbox, ticked each time it opens; unticked, the controller turns OFF at once with the valve left where it is, its position kept as after a close. Not homed, it turns OFF without moving the valve; already at 0 %, at once. The whole screen is black while it moves (a bar empties, with the seconds left, and STOP cancels to MANUAL) and while OFF ("Double tap to turn on"; two taps anywhere within 0.6 s show the MercShore iPC logo and a bar for about 1.5 s, then Home). OFF refuses moves, AUTO and homing (`ERR 213`); the pressure is still read. The limit switch tripping on the way to 0 % stops it in MANUAL with the limit switch question. OFF writes a parked entry (homed or not) to the saved-position log at once; at the next start, if the switch reads released, the count is taken without asking (Valve position: "Kept at shut down", or "Last saved, not homed" if it was not homed) and the controller stays OFF until turned on; pressed means NOT HOMED and nothing offered. There is no remote Shut down or turn on.
- **Faults** latch until Reset fault, which clears only those whose cause has gone: F-1 no pressure signal, F-2 excitation outside 9.5 to 10.5 V, F-3 switch not found while homing, F-4 switch still pressed after moving off it or pressed during an opening move, F-5 motion timeout, F-6 no pressure response in AUTO (five 30 s windows). F-3 to F-6 block moves until reset; F-1 and F-2 do not. Any fault in AUTO stops the valve and drops to MANUAL.
- **Warnings**: W-1 the limit switch question; W-2 outside the target band for 30 min in AUTO; W-3 controller hot (board temperature 55 °C, clears below 50 °C); W-4 switch pressed away from the seat; W-5 pressure above the sensor range in AUTO. W-2, W-4 and W-5 end AUTO, MANUAL with the valve idle. W-6 (driven the wrong way) was removed in 0.15.11: it tripped at low pressure, where the valve rightly rests near closed; a wrong pressure response setting now shows as W-2.

## Screens

480x272, ISA-101 style: grey for normal, colour only for abnormal states. Every screen has a header (Home and Back icons at the left, Home first, the title left-aligned after them, the mode on the right on a flat panel the header's height, from 10 px left of Start AUTO's column to the screen's edge: MANUAL, NOT HOMED or HOMING on grey, AUTO on dark blue, FAULT on red), a status strip and STOP on the bottom row. Under a popup the strip stays, updating and greyed; a tap on anything greyed (the dimmed page round the popup, the strip, a header icon) closes the popup as Back, except the limit switch question and the saved position offer, which need an answer. A keypad is a popup over the dimmed page: title and present value on the left, the entry on the right; an entry out of range on Set says "Out of range" with the range. On Home and the operator screens the strip shows the status line with the 5 min pressure sparkline; prompts, faults and messages take the whole strip, and every fault strip has Reset fault. A save icon in the header shows while a setting in use differs from the newest save; tapping it opens Review and save. On Home a power icon at the top left, where Home is on the other screens, opens Shut down ("Shut down the controller?" with a "Close the valve to 0 %" checkbox, and Cancel, Restart and Shut down); while shutting down and while OFF the whole screen is black (see Operation).

At power-up the screen is black from the first frame (the OFF screen): the backlight stays off until it is drawn. Turning it on shows the MercShore iPC logo on white with a bar that fills in uneven steps, then Home; the firmware version and build date are in the corner.

- **Home** ("<cryostat name> pressure controller"): Pressure tile (reading in mbar, log bar 0.1 to 1000 mbar, SP (setpoint) box in AUTO, Start AUTO otherwise), Valve tile (reading in %, bar; Go to box in MANUAL, Stop AUTO in AUTO, Home valve while not homed), and on the right the PID tile (KP, TI and KD in use; with the gain schedule on, the set in use with its KP and TI; a tap opens PID tuning), the IP tile ("IP address" over the address/prefix in use, for example 192.168.1.100/24; a tap opens System), Menu and STOP. No mode button: the header's panel shows the mode. A tap on the sparkline opens Trend.
- **Menu**: nine tiles, each with an icon over its name and no row captions: Trend, Events, PID tuning; Motor settings, Valve position, Pressure setup (the Calibrate sensor page); System, Diagnostics, Review and save (dark blue). There is no Settings page from 0.16.2; every page is one tap from the Menu.
- **Valve position**: how the position was set, output turns, the current homing stage, and the date or age of the last successful homing; Motor override (Move, Speed and Coils tabs; Switch tab to 0.16.18) is on Motor settings > Override from 0.16.17 (on this page before); the Ignore switch toggle on that tab (Switch pass to 0.16.20; 0.16.19; RAM only, off at power-up) stops the limit switch acting as the closing stop between its From and To edges (% open; saved settings from 0.16.21, 5 and 50 by default), for a valve that binds there and trips the force switch off the seat; below From and above To it works as always. Motor steps and the live limit switch are on Diagnostics. **Events**: latched faults (tap for detail and help) and the event log. **Diagnostics**: raw readings in three rows: pressure sensor (signal, before calibration, room pressure), valve motor (steps, limit switch, last move), controller (board temperature, excitation, how long the last move took).
- **Trend**: pressure (left axis), the setpoint in AUTO (dashed) and valve (right axis) over 1, 2, 5, 10, 30 min or 1 hour.
- **System**: cryostat name, IP address, MAC, Remote control switch, firmware version and build date, Restart.
- **Motor settings** (Valve setup to 0.16.1, Valve settings to 0.16.15): the valve's saved numbers on three tabs, Drive (run and start speeds, acceleration; first from 0.16.21), Travel (travel, direction, deadbands opening and closing) and Homing (back-off, return and speeds); these move nothing. A fourth tab, Override (from 0.16.17), has the Ignore switch toggle (0.16.19), its From and To edges (0.16.21) and the Motor override button, which asks first (Back returns to that tab). **PID tuning** (PID tab: KP, TI, KD, and a row to pick set 1 to 4; PID table tab: hysteresis and boundaries 1 to 3, the table always on by the pressure from 0.16.20 (an Off or On choice before); AUTO limits tab: AUTO min, pressure response, setpoint band, 0 to 10 mbar from 0.16.16, where 0 means no In band or Stable and W-2 still judges against 0.10 mbar). **Review and save**: last and previous save, unsaved changes, Save settings, Discard changes, Load settings (last save, previous save, factory). Numeric valve and tuning edits are drafts until Save settings, which checks them together and saves all or nothing. PID tuning drafts can be saved in AUTO; changed valve drafts need MANUAL. The cryostat name, IP address, Remote control switch and captured calibration take effect before saving and are kept across restarts only after Save settings; a changed IP address is used after a restart.
- **Calibrate sensor**: two-point calibration on the screen: vacuum, then air at the Pressure 21 Click room reading or a typed barometer reading. One sensor per board, the 15 psia Druck (the Choose sensor page, up to four sensors, went in 0.16.2); a replacement sensor is calibrated in place.

Display formats: pressure 1 decimal, valve % and keypad 2 decimals, dates DD/MM/YY. Fonts are Montserrat Medium 12, 16 and 26 px, and 40 px digits for the Home readings. The display is driven over the SSD1963's 16-bit 8080 bus, bit-banged on GPIOE (WR on PF11). LVGL heap: 64 KB in CCM in the default Ethernet build. The USB build adds a 24 KB pool in main RAM (88 KB total); the simulator uses that USB-build heap, so it does not establish that a screen fits the Ethernet build.

## Pressure measurement

The Druck output is ratiometric, so the firmware uses the ratio of signal to excitation (mV/V), read by the Power Monitor Click with 128 averages over 600 ms (30 cycles of 50 Hz mains), once a second. `cal.c` turns the ratio into pressure with a two-point calibration: a pumped-down vacuum point and an air point at the room pressure from the Pressure 21 Click (or a typed barometer reading). Both points are read live through the Power Monitor Click as wired, so its loading of the Druck output and its gain and offset sit inside the calibration. With no calibration saved, the nominal 0 to 100 mV at 10 V is used and the screen says so. The display shows 0.1 mbar; accuracy is set by the Druck (±0.04 % FS, ±0.41 mbar) and the calibration points.

## Storage

Serial flash (survives reflashing; neither the application nor mikroBootloader erases it):

| Address | Holds |
| --- | --- |
| 0x7EC000 to 0x7FBFFF | Saved valve position, a log of 16-byte entries over sixteen 4 KB sectors |
| 0x7FD000 and 0x7FE000 | Settings record (version 8), written to the two sectors in turn |
| 0x7FF000 | Legacy calibration record, read only if no settings record exists |

The trend and the event log are in RAM and empty after a restart.

## Build and flash

- `InitialTesting/pmc_config.h` sets the build. `PMC_LINK_ETHERNET 1` (default) gives the text port on TCP 8888 and Modbus TCP on 502, on a static IP address only: the one saved from Menu > System, else `PMC_IP`, 192.168.1.100/24 (mask 255.255.255.0). There is no gateway, so the controller answers on its own subnet only. `0` gives the text port on the USB COM port instead; the two cannot run together, because RMII and USB need different clocks from the same PLL.
- Network writes (set commands and Modbus writes) are taken only while Menu > System > Remote control is On (the default); while it is Off they are refused. `PMC_LINK_WRITE 0` and `PMC_MODBUS_WRITE 0` make the text port or Modbus read only whatever the switch says.
- Build Debug in NECTO (Clean first after `CMakeLists.txt` changes). The Release SSD1963 library leaves the display blank. `InitialTesting/CMakeLists.txt` keeps the SDK libraries and the hardware drivers at -O0 (`driver/display_lvgl.c` must stay -O0) and compiles LVGL, the pixel loop and the screens at -O2. About 663 KB of flash.
- The linker script `.meproject/setup/FileLinker/stm32f407zg.ld` is patched so `.ccmram` is `(NOLOAD)`, keeping the LVGL heap out of the flash image. If NECTO regenerates this file, re-apply the patch.
- NECTO's build folders, `compile_commands.json` and `.cache/` are not tracked, except `build-MM4/generated/`, which `tools/ui-preview` compiles. NECTO rewrites `.meproject/config.json`: before a pull, `git stash` or `git checkout -- InitialTesting/.meproject/config.json`.
- Flash over USB-C with mikroBootloader. Check `*IDN?` after flashing (the version is its last field).

## Text port commands

The text port and Modbus take the same operator command set, and nothing else: what an operator can use and judge away from the cryostat. The text port and Modbus also take the PID gains and the gain schedule (`PID?`, `PID SET`, `PIDS?`, `PIDS SET`, `SCHED?`, `SCHED SET`; Modbus registers 26 to 58). Homing, calibration, the other settings and the motor are on the touch screen. `InitialTesting/remote.c` is the text port; the Modbus registers and coils for the same commands are in `docs/modbus-register-map.md`.

Connect to TCP port 8888 (for example PuTTY in Raw mode), or the USB COM port in the USB build. One client at a time; a new connection replaces the old. Send upper case ASCII, a line ending in LF or CR LF, at most 96 characters. Replies end in CR LF.

The command names are whole words from firmware 0.16.12. To 0.16.11 every first word had four letters (`PRES`, `PRES SP`, `VALV`, `ALRM`, `EVNT`, `BARO`, `NETW`); those names now answer `ERR 100`, so the board and anything that talks to it (the GUI, the logger, a driver) change together.

- No echo, and nothing is sent unasked: faults, warnings and switch changes go to the event log (`EVENT?`) and show in `ALARM?` and `MODE?`.
- A query (ending in `?`) replies one line starting with its own words, the value then the unit: `PRESSURE 4.625 mbar`, `VALVE 34.12 %`, `VALVE NONE` when there is no value. Pressure 3 dp, setpoint 1 dp, valve 2 dp, room pressure 2 dp.
- A set command replies nothing when accepted, and `ERR <code> <text>` when refused. End a line that sets something with a query (`SETPOINT 5;MODE AUTO;MODE?`) to always get one line back.
- Commands can share a line with `;`: they run in order, the query replies come back on one line joined by `;`, and the first `ERR` ends the line. `EVENT?` is the only multi-line reply and must be on a line of its own.

| Command | Reply or action |
| --- | --- |
| `SETPOINT <mbar>` | The pressure setpoint, 0 to the sensor range, kept to 0.1 mbar. Does not start AUTO |
| `MODE AUTO` / `MODE MANUAL` | Start regulating after the entry checks / stop regulating, the valve stays where it is. Both refused (`ERR 213`) while OFF or shutting down: only the screen turns it on |
| `VALVE SET <%>` | Move the valve, 0 to 100 % in 0.1 steps; MANUAL only, valve homed |
| `STOP` | Stop the valve now; in AUTO it also ends AUTO. Cancels homing and a shut down. The only stop |
| `ALARM RESET` / `ALARM OK` | Clear the latched faults whose cause has gone / acknowledge the warning, as the screen's OK |
| `TIME SET <YYYY-MM-DD> <HH:MM:SS>` | Set the clock |
| `PID SET <kp>,<ti>[,<kd>]` | Set and save the gains of the PID set in use (the PID table is always on from 0.16.20), as Save on the PID tuning page: KP in %/mB, TI in min (0 off), KD in %/(mB/s) (0 off, left as it is when not given), 2 dp. KP 0.01 to 100, TI and KD 0 to 100. Taken in AUTO too, at the next control step. Each change is a settings save in flash, so send it when the gains change, not every poll; gains equal to the present ones write nothing |
| `PIDS SET <set>,<kp>,<ti>[,<kd>]` | As `PID SET`, for PID set 1 to 4 (set 4 from 0.16.14) |
| `SCHED SET PV,<b1>,<b2>,<b3>,<h>` | The PID table (gain schedule), saved as `PID SET`: always on the pressure (`OFF`, set 1 always, removed in 0.16.20: `ERR 102`; `SP`, the setpoint, removed in 0.15.20: `ERR 101`); boundaries B1, B2 and B3 in mbar (1 dp, 0.1 to 1000; set 1 below B1, set 2 to B2, set 3 to B3, set 4 above; B3 0 for three sets), hysteresis H in % of the boundary (0 to 40). B2 must be above B1, and B3 (unless 0) above B2, by more than H either side (`ERR 102`). B3 from 0.16.14: the four-number form answers `ERR 101` |
| `PRESSURE?` | `PRESSURE 4.625 mbar` or `PRESSURE NONE` |
| `SETPOINT?` | `SETPOINT 5.0 mbar` or `SETPOINT NONE` |
| `MODE?` | `MODE MANUAL`, `MODE AUTO`, `MODE HOMING`, `MODE NOT HOMED`, `MODE SHUTDOWN` (going to 0 % for Shut down) or `MODE OFF` |
| `VALVE?` / `VALVE MOVING?` | `VALVE 34.12 %` or `VALVE NONE` / `VALVE MOVING 1` while moving or homing, else 0 |
| `BAROMETER?` | `BAROMETER 1013.25 mbar` or `BAROMETER NONE` (Pressure 21 Click not fitted or not reading) |
| `ALARM?` | `ALARM NONE`, or the active alarms joined by `\|`, faults first: `<tag>,<0\|1>,<text>`. For a fault, 1 when `ALARM RESET` clears it now; for a warning, 1 when `ALARM OK` clears it |
| `EVENT? [<n>]` | `EVENT <n>`, then n lines `<age s>,<tag>,<text>`, newest first; default 1, at most 100 |
| `NETWORK?` | `NETWORK STATIC,<address>,<mask>,<MAC>`; `NETWORK NONE` in the USB build |
| `TIME?` | `TIME 2026-10-27 11:05:00` or `TIME NONE` |
| `PID?` | `PID 0.12,0.82,0.00`: KP, TI, KD of the PID set in use (factory set 2, at 6 to 10 mbar) |
| `PIDS?` | `PIDS 2,0.06,0.70,0.00,0.14,1.50,0.00,0.50,1.00,0.00,0.30,0.25,0.00`: the set in use (in AUTO the one the schedule has switched to, else the one AUTO would start with), then KP, TI, KD of sets 1 to 4 |
| `SCHED?` | `SCHED PV,6.0,10.0,14.0,15`: the PID table (always `PV` from 0.16.20; `OFF` could show before), B1, B2, B3 in mbar, H in % |
| `*IDN?` | `MercShore,iPC,<chip id>,<version>` |

Error codes: 100 unknown command (anything not in the table above when network writes are enabled; with writes off, unrecognised input also gets 211), 101 expected form, 102 outside range, 103 line too long, 104 `EVENT?` not on its own line, 201 homing, STOP to cancel, 202 switch to MANUAL first, 203 reset the fault first, 204 home the valve first, 205 no valid pressure, 206 set a setpoint first, 207 setpoint outside the sensor range, 208 pressure above the sensor range, 209 control settings not set, 210 not reset, cause still present, 211 read only over the network, 212 the valve cannot move now (the limit switch, Close or Open held on the screen, the switch question waiting, a calibration point being taken) or the clock is not running, 213 controller off or shutting down (turned on at the screen).

## Tools

- `tools/pmc-gui/pmc_gui.py <IP> [--ls336 <IP>]` serves the board's PC tools on one page at http://localhost:8080: live chart, setpoint, AUTO / MANUAL / STOP, PID gains, the PID autotune (a relay test per setpoint, first gains from a fit of the response, checks that refine KP and TI, then the board's three PID table sets, written on request), the event log, the clock and a command line, with the Lake Shore 336 temperatures beside the pressure. Every sample and every autotune result is saved to `pmc-logs` as it comes. Standard library Python, nothing to install.
- `tools/lakeshore/ls336_log.py <IP>` logs Lake Shore 336 temperatures (A heat exchanger and D cold head by default) over Ethernet to a CSV, without the GUI. Standard library Python.
- `tools/logger/log_druck.py <IP or COM>` polls pressure, target, valve, mode, alarms and room pressure once a second into a CSV and reconnects on its own.
- `tools/gain-schedule/pmc_autotune.py` is the autotune the GUI runs (and the text-port link under it); `test_pmc_autotune.py` tests it on a valve model without a board. Python 3 only, no pip.
- `tools/ui-preview/build.sh` renders the screens to PNG on a PC with the real settings store over a flash held in RAM, and checks the LVGL heap returns after opening and leaving screens.
- `tools/ctrl-test/run.sh` runs `ctrl.c` on a PC against a model of the flow line (helium pot, needle valve, pump, motor ramp, closed switch): setpoint steps, helium off (F-6), pump off (W-2), a slipping drive (F-6), and the entry refusals.
- `tools/cfg-test/run.sh` tests the settings store and the saved valve position against a flash held in RAM.
- `tools/modbus-test/run.sh` tests the Modbus server and register map on a PC; `mb.sh` reads and writes registers from Git Bash.
- `tools/baro-test/run.sh` tests the Pressure 21 Click conversion and plausibility window.
- `tools/images/mkimg.py` makes the logo (`ui_img_logo.c`) from `mercshore-ipc-logo.png`.
- `tools/xbuild/build.sh [Release|Debug]` cross-builds the firmware on Linux (cmake, ninja, `gcc-arm-none-eabi`) with the mikroSDK libraries in `.meproject` and prints flash and RAM use. For size checks only; the flashed build is NECTO's.
- `simulator/wasm/build.sh` rebuilds the simulator; `node simulator/tests/run.js` runs its tests, and `simulator/tests/tcp-board.js` serves the simulated board on port 8888, on the bench cryostat fitted to the bench logs (`--physics` for the physics flow line). It homes through the simulator test hook and starts in MANUAL at 5 % open; `node simulator/tests/tcp-board-test.js` checks its startup and remote-write switch.
