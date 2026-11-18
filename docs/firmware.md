# iPC firmware

This document describes the firmware in `firmware/`: the hardware it runs on, its structure, the pressure measurement, what it stores, and how to build and flash it. Operation is in `docs/operation-manual.md`, the text port in `docs/text-port-commands.md` and Modbus in `docs/modbus-register-map.md`.

The firmware is a NECTO Studio 7 project on the mikroSDK with LVGL 9.4. Its version is `IPC_FW_VERSION` in `firmware/ipc_config.h`; `*IDN?` and Menu > System report it.

## Hardware

The controller is one MikroE development board with a socket shield and four Click boards. MikroE calls its small add-on modules "Click boards"; each plugs into a mikroBUS socket (two 1x8 headers carrying SPI, I2C, UART, PWM, an analog pin, reset, 3.3 V and 5 V). The names below are MikroE's product names. Schematics and wiring diagrams are in [`docs/hardware/`](hardware/).

### Boards

| Board | MikroE part | Main chip | Where | What it does in the controller |
| --- | --- | --- | --- | --- |
| Mikromedia 4 for STM32 Capacitive | MIKROE-3618 | STM32F407ZG (168 MHz, 1 MB flash, 192 KB RAM); SSD1963 display controller; LAN8720A Ethernet PHY; SST26VF064B 8 MB serial flash | Main board | Runs the firmware. 4.3 in 480x272 capacitive touchscreen, 12 V terminal, USB-C (power and flashing), real-time clock |
| Mikromedia 4 Capacitive Shield | MIKROE-3631 | none | Plugs onto the back of the main board | Four mikroBUS sockets, S1 to S4, and the RJ45 for Ethernet |
| Adapter Click | MIKROE-1432 | none (a pass-through) | Socket S1 | Raises the Pressure 21 Click above the shield and brings every socket pin out on a 2x5 IDC10 header. The valve limit switch is wired to this header |
| Pressure 21 Click | Pressure 21 Click | Bosch BMP581 barometric sensor | On top of the Adapter Click in S1 | Room (atmospheric) pressure. Used as the air point of the transducer calibration and shown on Diagnostics and `BAROMETER?` |
| Stepper 3 Click | MIKROE-2035 | ULN2003 Darlington array | Socket S2 | Drives the valve's unipolar stepper motor from the 12 V motor supply |
| Boost 10 Click | MIKROE-6927 | LT8337 boost converter, TPL0501 digital potentiometer | Socket S3 | Makes the 10 V excitation for the Druck transducer from the socket's 5 V |
| Power Monitor Click | MIKROE-4810 | TI INA228 | Socket S4 | Measures the Druck output (shunt input, ±163.84 mV) and the excitation (bus input), so the firmware can use their ratio. Its die temperature shows on Diagnostics |

Off-board parts: the Druck UNIK 5000 transducer (PDCR50G1, 15 psia absolute, 10 mV/V, M12 4-pin), the existing Oxford Instruments needle valve (Portescap 26M048B2U or 35M048B2U unipolar stepper, PK22 344:1 gearbox, NC limit switch, Fischer 7-pin), and a PoE splitter giving regulated 12 V.

### Connections

| From | To | Pins |
| --- | --- | --- |
| Druck M12 pin 1, +Supply | Boost 10 Click VOUT and Power Monitor Click VBUS | |
| Druck M12 pin 2, +Out | Power Monitor Click IN+ | |
| Druck M12 pin 4, -Out | Power Monitor Click IN- | |
| Druck M12 pin 3, -Supply | Common 0 V | |
| Valve Fischer pins 1 to 4, coils | Stepper 3 Click outputs A to D | The firmware drives them in the order 0-2-1-3 (found with the 26M048B2U motor); nothing on the screen changes it. Direction is on Motor > Settings |
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

## Structure

`main.c` initialises everything (version and reset cause, clock, LVGL and display, calibration, Clicks, limit switch, valve, settings, saved position, screens, links), shows the OFF screen from the first frame, starts the independent watchdog (about 3 to 4 s), then runs one loop with no RTOS:

1. Links, touch (every 5 ms), clock, limit switch, stepper and valve tasks, saved position.
2. Once a second: `sense_second()` reads the Power Monitor Click and the Pressure 21 Click, converts to pressure, updates the faults, the trend and AUTO.
3. One command line from the text port, if any.
4. `lv_timer_handler()` (screens refresh at 2 Hz, the fast parts at 10 Hz), the watchdog kick, 1 ms delay.

Steps are timed by TIM7 interrupts, so the motor runs smoothly while the screen draws.

| File | What it does |
| --- | --- |
| `ipc_config.h` | Build options: the link, limit input polarity and pull-up, the Pressure 21 Click, network ports and the default IP address, the version |
| `clicks.c` | Boost 10 Click excitation set-up and 11.5 V trip, Power Monitor Click reads, Druck plausibility, pressure from the calibration |
| `baro.c` | Pressure 21 Click room pressure: start-up, reads, plausibility window, retry when lost |
| `cal.c` | Druck calibration: two points (vacuum and air) of ratio (mV/V) against pressure, a straight line through them; the five-reading capture |
| `sense.c` | The once-a-second read |
| `limit.c` | Limit switch input: a trip counts at once, the release is debounced (20 ms) |
| `stepper3.c` | Full-step sequence, ramps, absolute moves with retarget, the limit stop, motion timeout |
| `valve.c` | Modes (NOT HOMED, HOMING, MANUAL, AUTO, SHUTDOWN, OFF), homing, Set as %, Shut down, % position, STOP, fault latches, the event log (100 kept), a millisecond clock that runs past the 49.7 day tick wrap, runs AUTO |
| `ctrl.c` | AUTO control step: PID with KP on the pressure only, the PID table of four zones, anti-windup, deadband, reversal hold and the response monitor (no hardware calls; `tools/ctrl-test` runs it on a PC) |
| `valvepos.c` | Saves the step count at standstill and every 2 s while moving, so a restart can offer it |
| `cfg.c` | Settings record in serial flash, two sectors in turn, CRC32, read back before it counts; edits in use at once, Save, Undo, Factory reset |
| `trend.c` | Trend rings in RAM: 600 one-second samples and 360 ten-second means, on fixed one-second slots |
| `extflash.c` | SST26 serial flash driver |
| `rtclock.c`, `sysinfo.c` | Clock; version and reset cause |
| `link.c`, `ethlink.c`, `eth_hw.c` | Text port and Modbus over TCP; clients dropped when the cable is pulled |
| `remote.c` | The command set on the text port |
| `modbus.c`, `mbmap.c` | Modbus TCP server on port 502: the protocol (FC1, FC3, FC5, FC16) and the register map (`docs/modbus-register-map.md`; `tools/modbus-test` runs both on a PC) |
| `ui.c` | Screen framework: header, status strip, STOP, buttons, cards, keypad, pick lists, questions; the Power options question, the limit switch question, the start-up offer; the OFF, closing and turning on screens; the backlight |
| `ui_home.c` | Home and Menu; the Home valve and Set as % questions |
| `ui_valve.c` | Events (with the fault detail) and Diagnostics |
| `ui_settings.c` | PID, Valve, Motor, Pressure sensor and System pages, the IP address page; Save, Undo, Factory reset and Restart |
| `ui_trend.c` | Trend page and the Home sparkline |
| `motor_screen.c` | Motor > Jog: Close, Limit override and Open |
| `ui_font_*.c`, `ui_img_logo.c` | Fonts and the logo shown when turned on |
| `driver/` | Display and touch ports for LVGL and the backlight level; `display_px.c` is the pixel loop |
| `fixed.c` | Fixed-point text and parsing (lv_snprintf has no float) |

## Screens

480x272, ISA-101 style: grey for normal, colour only for abnormal states. Every screen has a header, a status strip and STOP on the bottom row. The header has Home and Back icons at the left (on Home the power icon in Home's place, and no Back), the title, the yellow save icon while a setting in use is not saved, and the mode on the right on a flat panel the header's height: MANUAL, NOT HOMED or HOMING on grey, AUTO on dark blue, FAULT on red. Under a popup the strip stays, updating and greyed; a tap on anything greyed (the dimmed page round the popup, the strip, a header icon) closes the popup as Back, except the limit switch question and the start-up offer, which need an answer. A keypad is a popup over the dimmed page: title and present value on the left, the entry on the right; an entry out of range on Set says "Out of range" with the range. On Home and the operator pages the strip shows the status line with the 5 min pressure sparkline; prompts, faults and messages take the whole strip, and every fault strip has Reset fault. A set taken over the network shows on the strip for 5 s ("Network: setpoint 7.5 mbar").

At power-up the screen is black from the first frame (the OFF screen): the backlight stays off until it is drawn. Turning it on shows the MercShore iPC logo on white with a bar that fills in uneven steps, then Home; the firmware version and build date are in the corner.

Display formats: pressure 1 decimal, valve % and keypad 2 decimals, dates DD/MM/YY; mB on the screens, mbar on the text port. Fonts are Montserrat Medium 12, 16 and 26 px, and 40 px digits for the Home readings. The display is driven over the SSD1963's 16-bit 8080 bus, bit-banged on GPIOE (WR on PF11); the backlight level is the SSD1963's PWM. LVGL heap: 64 KB in CCM; the simulator has the same 64 KB.

Each screen is described in `docs/operation-manual.md`, section 5, and shown in `docs/screens/`.

## Pressure measurement

The Druck output is ratiometric, so the firmware uses the ratio of signal to excitation (mV/V), read by the Power Monitor Click with 128 averages over 600 ms (30 cycles of 50 Hz mains), once a second. `cal.c` turns the ratio into pressure with a two-point calibration: a pumped-down vacuum point and an air point at the room pressure from the Pressure 21 Click (or a typed barometer reading). Both points are read live through the Power Monitor Click as wired, so its loading of the Druck output and its gain and offset sit inside the calibration. With no calibration saved, the nominal 0 to 100 mV at 10 V is used and the screen says so. The display shows 0.1 mbar; accuracy is set by the Druck (±0.04 % FS, ±0.41 mbar) and the calibration points.

## Storage

Serial flash (survives reflashing; neither the application nor mikroBootloader erases it):

| Address | Holds |
| --- | --- |
| 0x7EC000 to 0x7FBFFF | Saved valve position, a log of 16-byte entries over sixteen 4 KB sectors |
| 0x7FD000 and 0x7FE000 | Settings record, written to the two sectors in turn with a CRC32 and read back before it counts. A record of another version is not read: the controller starts on the factory settings and says so in Events |

The trend and the event log are in RAM and empty after a restart.

## Build and flash

- `firmware/ipc_config.h` sets the build. The text port is on TCP 8888 and Modbus TCP on 502, on a static IP address only: the one saved from Menu > System, else `IPC_IP`, 192.168.1.100/24 (mask 255.255.255.0). There is no gateway, so the controller answers on its own subnet only. `IPC_LINK_ETHERNET 0` is for host builds only (the simulator and the screen previews); there is no USB COM port build.
- Network writes (set commands and Modbus writes) are taken only while Menu > System > Network control is On (the default); while it is Off they are refused. That switch is the only gate.
- Build Debug in NECTO (Clean first after `CMakeLists.txt` changes). The Release SSD1963 library leaves the display blank. `firmware/CMakeLists.txt` keeps the SDK libraries and the hardware drivers at -O0 (`driver/display_lvgl.c` must stay -O0) and compiles LVGL, the pixel loop and the screens at -O2. About 659 KB of flash; static RAM about 96 KB of the 128 KB SRAM, with the LVGL heap in the 64 KB CCM.
- The linker script `.meproject/setup/FileLinker/stm32f407zg.ld` is patched so `.ccmram` is `(NOLOAD)`, keeping the LVGL heap out of the flash image. If NECTO regenerates this file, re-apply the patch.
- NECTO's build folders, `compile_commands.json` and `.cache/` are not tracked, except `build-MM4/generated/`, which `tools/ui-preview` compiles. NECTO rewrites `.meproject/config.json`: before a pull, `git stash` or `git checkout -- firmware/.meproject/config.json`.
- Flash over USB-C with mikroBootloader. Check `*IDN?` after flashing (the version is its last field).
