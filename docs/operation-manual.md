# iPC operation manual

Firmware 0.18.0.

This manual covers day-to-day use of the iPC pressure controller: power-up, Shut down, homing, MANUAL and AUTO, calibration, faults and warnings, settings, and remote access over Ethernet. It is written for the operator at the cryostat.

| Field | Value |
| --- | --- |
| Equipment | iPC pressure-based needle-valve controller for helium-flow cryostats such as the CF-11 and CF-12 |
| Hardware | MikroE Mikromedia 4 for STM32F4 with the Adapter, Pressure 21, Stepper 3, Boost 10 and Power Monitor Click boards; Druck UNIK 5000 15 psia transducer; geared unipolar stepper on the needle valve |
| Firmware | 0.18.0 |
| Sources | Firmware source in `firmware/`, `docs/firmware.md`, `docs/text-port-commands.md` and `docs/modbus-register-map.md` |
| Status | Tested on the board, on a bench cryostat |
| Exclusions | Build and flash procedure, firmware structure (`docs/firmware.md`), the simulator (`simulator/README.md`), PID tuning theory |

Contents: 1 Overview, 2 Safety and limits, 3 Hardware and connections, 4 Power-up, Shut down and the screen light, 5 Screens, 6 MANUAL and AUTO, 7 Homing and the valve position, 8 Sensor calibration, 9 Faults and warnings, 10 Settings, 11 Remote access, 12 Maintenance.

## 1. Overview

The controller holds the pressure on the cryostat pumping line at a target the operator sets, by driving the needle valve's stepper motor. The pressure is measured by the Druck transducer on the line. Pressure is a rough proxy for the helium flow through the cryostat. The flow itself is not measured, so the readings and screens are always in mbar (mB on the screen) and never labelled as flow.

What the operator does:

- Turns the controller on with a double tap and homes the valve after power-up, so the controller knows where closed is.
- Sets the target pressure (the setpoint) for the cryostat, from experience. Typical targets are 1 to 25 mbar.
- Switches between MANUAL (the operator positions the valve in percent open) and AUTO (the controller regulates the pressure to the setpoint).
- Watches the trend. A steady pressure matters more than the exact value. The sample temperature is watched on another instrument.
- Clears faults after fixing their cause.
- Shuts the controller down with the power icon on Home before switching it off (section 4).

The operator can also change the PID gains (Menu > PID, section 10), in AUTO too. The factory gains are a starting point; Home shows the ones in use.

The controller has six modes, shown at the right of the header on every screen, on a flat panel the header's height (grey, dark blue in AUTO; with a latched fault it reads FAULT, in red):

| Mode | Meaning |
| --- | --- |
| NOT HOMED | The valve position is not known. Nothing moves at power-up until the operator homes the valve or sets its position |
| HOMING | The valve is finding the closed limit switch |
| MANUAL | The operator positions the valve. The pressure is shown but not regulated |
| AUTO | The controller moves the valve once a second to hold the setpoint |
| SHUTTING DOWN | Shut down is moving the valve to 0 %. The screen is black with a bar and STOP (the header is hidden; `MODE?` answers `SHUTDOWN`) |
| OFF | Every power-up starts here, and Shut down ends here. The motor coils are off and nothing moves. The screen is black: a double tap turns the controller on |

The controller is operated from its 4.3 inch touchscreen. It can also be read and commanded over Ethernet: a text command port on TCP port 8888 and a Modbus TCP server on port 502, for a supervisory system or a person at a terminal (section 11). The USB-C port is for power and flashing only.

## 2. Safety and limits

The controller has no hardware interlock. The closing stop is the firmware reading the valve's limit switch before every closing step. This is a design decision. The points below follow from it.

- **The limit switch can trip early.** It is a force-actuated normally closed contact. Ice on the valve, or a stiff drive, can press it before the needle reaches the seat. The controller cannot tell an early trip from the closed valve. When the switch trips during a closing move, or reads pressed with the valve away from the closed end, the controller stops and asks the operator (W-1, section 9). Only a person at the cryostat can judge which it was.
- **STOP** on the bottom row of every screen stops the valve at once and holds it. In AUTO it also switches to MANUAL. It also cancels homing, a shut down and a calibration capture. Nothing latches, so operation can resume straight away. STOP acts on the first touch even when the screen is dimmed (section 4), and works with another finger on the glass.
- **Nothing moves at power-up.** Every power-up and restart starts OFF, and only a double tap on the screen turns the controller on. The network cannot turn it on, so AUTO does not resume after a power cut until someone is at the screen. The valve is NOT HOMED until the operator homes it or sets its position. The controller never uses a saved position on its own, with one exception: after Shut down, if the limit switch reads released at power-up, it keeps the position it shut down at (section 4). The valve does not move either way.
- **Shut down before switching off.** Shut down moves the valve to 0 % and records the position, so the next power-up needs no homing. Switching off without it is safe (the valve stays where it is) but the next power-up asks for the position or a homing.
- **Limit override** (Menu > Motor > Jog) drives the valve closed past a pressed limit switch. It takes two fingers: hold Limit override, then press Close with another finger. Holding Limit override alone does nothing. A closing override moves only while both fingers are held and stops at once when either lifts. Use it only when the switch is known to be stuck and the valve is known to be off the seat.
- **Disable switch** (Menu > Valve > Limit) stops the limit switch acting between two openings (5 to 50 % by default) for a valve that binds there and trips the switch off the seat. While it is On the valve can close through that range with the switch pressed. It is Off after every restart (section 7).
- **Motor supply.** The Stepper 3 Click has no current limit. The motor supply must be a regulated 12 V. The firmware does not measure it: losing motor power does not directly end AUTO, and the counted position can keep changing while the motor is stationary. Check the supply and home again if the position is in doubt. Do not feed 12 V into the Boost 10 Click, which takes 5 V from the board and makes the 10 V Druck excitation.
- **Network control.** While Menu > System > Network control is On (the default), anyone who can reach the controller's IP address on port 8888 or port 502 can set the setpoint, move the valve in MANUAL, start and end AUTO, stop, reset faults and change the PID gains and table. With it Off the network can only read, and a network STOP is refused too. Keep the controller on the instrument network only.

Operating limits:

| Item | Limit | Source |
| --- | --- | --- |
| Board supply | 12 V on the terminal (10.6 to 14 V) | Mikromedia 4 |
| Druck excitation | About 10 V from the Boost 10 Click. The reading is the ratio of the signal to the excitation, so it holds at any excitation from 7 V up. Under 7 V, or tripped above 11.5 V, there is no reading: fault F-1 | Firmware |
| Pressure range | 0 to 1034 mbar absolute (15 psia sensor). Display resolution 0.1 mbar; accuracy set by the Druck (±0.04 % FS, ±0.41 mbar) and the calibration points | Druck datasheet |
| Setpoint | 0.0 to the sensor range, kept to 0.1 mbar. Operators use 1 to 25 mbar | Firmware |
| Valve travel | 0 to 100 % of 66048 steps (4 output turns of 16512 steps) by default. 0 % is the Return point just above the closed valve; the closed valve reads about -0.6 % with the default Return | Firmware |

## 3. Hardware and connections

| Part | What it does | Connection |
| --- | --- | --- |
| Mikromedia 4 for STM32F4 | Controller and 4.3 inch 480 x 272 touchscreen. Runs the control loop, the screens, the Ethernet link and the Modbus server | 12 V terminal (2.54 mm pitch), USB-C for flashing, RJ45 Ethernet on the shield |
| Stepper 3 Click, socket S2 | Drives the unipolar six-lead valve stepper (PK22, 344:1 gearbox) at 12 V, full steps, coils in the order 0-2-1-3. No current limit | Motor cable to the Click's four coil outputs and the 12 V motor supply to its COM |
| Limit switch, socket S1 | One normally closed dry contact to ground, opens at the closed end of travel | Socket TX (PD5, internal pull-up) and GND. On the Adapter Click's 10-pin header that is pad 8, printed RX, and pad 10. The Adapter Click prints its UART pad names from the plugged-in Click's side, so pad 8 printed RX is the socket's TX |
| Boost 10 Click, socket S3 | Makes about 10 V from the board's 5 V for the Druck excitation. Trips above 11.5 V. Never feed it 12 V | SPI1 |
| Power Monitor Click, socket S4 | Reads the Druck output and the excitation, 128 averages over 600 ms, once a second. Its die temperature shows on Diagnostics | I2C2, address 0x4A |
| Druck UNIK 5000 PDCR50G1 | 15 psia absolute pressure transducer, 10 mV/V, insulated from its case | M12: pin 1 +Supply to Boost 10 Click VOUT and Power Monitor Click VBUS; pin 2 +Out to IN+; pin 4 -Out to IN-; pin 3 -Supply to common 0 V |
| Pressure 21 Click on the Adapter Click, socket S1 | Room pressure for the air point of the calibration, shown as Barometer on Diagnostics | I2C2, address 0x46 |
| On-board serial flash | Settings, calibration and the saved valve position. Survives reflashing | |
| On-board real-time clock | Time stamps for the event log and saves. Survives a reset, and a power-off with a cell fitted | |

Power: the board takes 12 V on its terminal, with USB-C and the Li-Po connector as lower-priority sources; SW1 must be on. The motor supply is a regulated 12 V from a PoE splitter. Both the board and the motor need their 12 V; the board alone runs the screens, the link and the pressure reading.

Earth: bond one 0 V point of the controller to the chassis earth. On the bench the board floating on the PoE splitter's 12 V put noise on the pressure reading. One 0 V to earth bond removed it.

Reference gauge for calibration: a Pfeiffer TPG 202 Neo (USB-C readout only). Its vacuum reading is typed into the controller by hand (section 8).

Coil order: the firmware drives the coils 0-2-1-3, as found on the bench. Nothing on the screen or the network changes it; a motor wired in another order is fixed at the plug. Direction (Menu > Motor > Settings) reverses the motor.

Socket pin details are in `docs/firmware.md` and `firmware/clicks.h`, `limit.h` and `extflash.h`; the wiring drawings are in `docs/hardware/wiring/`.

## 4. Power-up, Shut down and the screen light

At power-up the controller loads the newest saved settings, starts the pressure reading and shows the OFF screen: black, a grey power symbol and "Double tap to turn on". The valve does not move. Two taps anywhere on the screen within 0.6 s turn it on: the start-up logo and bar for about 1.5 s, then Home, in NOT HOMED unless the position was kept from Shut down.

1. Check the header. It reads NOT HOMED. Menu > System shows the firmware version and build date, the IP address, the MAC address and the clock. Menu > Diagnostics shows the live sensor, motor and board readings.
2. Check the pressure. The Pressure tile shows the line pressure in mB. Dashes mean no valid reading (section 9, F-1). Menu > Pressure sensor shows whether a saved calibration is in use (section 8).
3. If a saved position is on offer, a question asks for the valve position before anything else. Its first line says why: "Power lost without shut down.", "Power lost in AUTO at X mB." or "Restarted.". Then "Last saved position X %." (or, if the position was saved during a move, "Valve was moving: last saved position X %, within ±E %."), and "Home valve closes the valve, up to N min.". The answers are Home valve, Set as % and Use X % (the filled button). Use X % is quick but is not checked against the limit switch. Home valve is the checked option.
4. Home the valve (section 7) if it was not done in step 3. The header then reads MANUAL, the valve reading is 0.00 % and the strip reads "Valve idle".
5. Set the valve where the cryostat needs it, in MANUAL, or start AUTO (section 6).

If the controller was shut down with Shut down and the limit switch reads released at power-up, steps 3 and 4 are skipped: the position is kept, the controller still starts OFF, and once turned on Home opens in MANUAL with the valve where it was left (0.00 % if Shut down closed it). Nothing is asked, and the valve can be moved or put in AUTO at once. Valve > Position then reads Homed: Before Off if the valve was homed before, or Homed: No if its position had been set with Set as %. If the switch reads pressed (the valve was closed further while the power was off, or the switch or its wire is at fault), the position is not kept and nothing is offered: home the valve.

After a restart the setpoint is not kept; Start AUTO takes the pressure shown as the setpoint. The saved settings and calibration are kept. Settings in use but not saved are lost. The trend and the event log are in RAM and start empty.

The real-time clock keeps time through a reset. System > Date and time shows it, or "Not set". If the dates on saves and events are wrong, set the clock over the text port with `TIME SET` (section 11). There is no clock setting on the screen.

### Power options: Shut down and Restart

Before switching the controller off, tap the power icon at the top left of Home (where Home is on the other screens), or Power on Menu > System. The Power options question has a checkbox, "Close the valve on shut down", ticked each time the question opens, and Cancel, Restart and Shut down. It is refused in AUTO ("Stop AUTO first"): stop AUTO first. Tap the checkbox row to untick it: Shut down then turns the controller OFF at once with the valve left where it is, and the position is kept just the same.

1. With the box ticked, Shut down moves the valve to 0 % at the run speed (400 steps/s by default, about 50 s from 30 % open). It does not close onto the limit switch.
2. While it moves the whole screen is black: "Closing valve", a bar filling as the valve closes and the time left. STOP, in its usual place, cancels it: MANUAL, the valve where it stopped, and the note "Shut down cancelled. Valve at X %".
3. At 0 % the motor coils switch off and the controller is OFF, the same black screen as at power-up. The position is written to the serial flash at once. It is now safe to switch off.
4. While OFF the controller still reads the pressure and answers queries, but refuses moves, AUTO and homing. A double tap turns it on: the start-up logo and bar, then Home in MANUAL.

If the valve is not homed, there is no 0 % to go to: the question has no checkbox and reads "Valve not homed: it remains in position.", and Shut down turns the controller OFF with the valve where it is. If the position was set with Set as % or taken from the saved position, the checkbox row adds "Position unverified: 0 % may not be closed." During homing Shut down is greyed ("Homing in progress: STOP to cancel."). If the valve is already at 0 %, it turns OFF at once. If the limit switch trips on the way to 0 % (ice, a switch tripping early, or a count that slipped), Shut down stops in MANUAL, the strip reads "Shut down stopped: limit switch pressed before 0 %" and the limit switch question follows (W-1, section 9). With F-2 or F-3 latched, closing the valve is refused (reset the fault first); Shut down with the box unticked still works.

Restart stops the valve (AUTO ends, homing is cancelled), waits 2.5 s so the valve position is saved at standstill, and restarts the controller. It starts OFF as at power-up, and once turned on offers the saved position with "Restarted.". Restart has no question of its own.

Shut down, Restart and turning on are on the screen only; the network can do none of them.

### Screen light

The backlight is rated for 20,000 to 40,000 hours, so the controller dims it when nobody is using the screen:

- After 10 minutes with no touch the screen dims to 10 %. The first touch brightens it and does nothing else: it reaches no button until the finger lifts. STOP is the exception and always stops on the first touch.
- A new fault, a new warning, the limit switch question, the saved position offer, any message on the strip and a set taken over the network also brighten it.
- While OFF the screen goes dark 30 s after the last touch (or the power-up). A tap lights it and counts as the first tap of the double tap.
- While shutting down the screen stays lit.

At power-up the backlight comes on only once the OFF screen is drawn, so the panel shows no white or random pixels first.

### First use

A new board or a new Druck sensor also needs, once: a two-point calibration of the sensor (section 8; the board holds one 15 psia sensor), the valve travel, steps per turn and motor direction checked on Menu > Valve and Menu > Motor, the PID gains and table on Menu > PID if the factory ones do not suit the cryostat, and Save.

## 5. Screens

Every screen has the same frame:

- **Header.** At the left the Home and Back icons (Home first); on Home the power icon takes Home's place and there is no Back. Then the page title. Left of the mode panel the yellow save icon shows while a setting in use differs from the last save; a tap asks "N unsaved changes. Save?" with Cancel, See more (the list of what changed, each with its saved value) and Save. At the right the mode panel: MANUAL, NOT HOMED or HOMING on grey, AUTO on dark blue, FAULT on red.
- **Status strip** on the bottom row, with STOP at its right. On Home and the operator pages it shows the valve's state (Valve idle, Moving to X %, Position unverified, or in AUTO Regulating to, In band or Stable at the setpoint) with the 5 minute pressure sparkline; a tap on the sparkline opens Trend. Prompts, faults and messages take the whole strip, and every fault strip has Reset fault. A set taken over the network shows for 5 s ("Network: setpoint 7.5 mbar").
- **Popups.** A keypad, a question or a list opens over the dimmed page. Under it the strip stays visible, updating and greyed, and Home and the power icon are greyed. Back, or a tap on anything greyed (the dimmed page, the strip, a header icon), closes it as Cancel does: nothing is applied and a keypad entry is dropped. Two questions need an answer and stay: the limit switch question and the saved position offer at start-up. A question ignores taps for its first second, so a tap meant for the page under it does not answer it.
- **Side column.** On the settings pages the right column holds Help, Save and Undo.

While shutting down and while OFF the whole screen is black (section 4). Colour follows the ISA-101 convention: grey when normal, colour only for an abnormal state.

| Screen | How to reach it | What it shows and does |
| --- | --- | --- |
| Home | Home icon | Title "Pressure controller". Pressure tile: the reading in mB, a log bar 0.1 to 1000 mB; in AUTO the SP (setpoint) box, otherwise the Start AUTO button. Valve tile: the reading in %, a bar 0 to 100 %; in MANUAL the Go to box, in AUTO the Stop AUTO button, while not homed the Home valve button. A yellow edge round the Valve tile while the limit switch is pressed, in any mode; F-2 or F-3 gives a red edge instead. While the Disable switch is On its range shows yellow on the valve bar. On the right, from the top: the PID tile (KP, TI and KD of the zone in use; TI 0 reads Off; a tap opens PID), the IP tile ("IP address" over the address and prefix in use, for example 192.168.1.100/24; a tap opens System), Menu and STOP |
| Menu | Menu on Home | Nine tiles, each an icon over its name: Trend, Events, PID; Motor, Valve, Pressure sensor; System, Factory reset, Diagnostics. Every page is one tap from here. Factory reset has no page (section 10) |
| Trend | Menu > Trend, or a tap on the sparkline | Pressure (left axis), the setpoint while in AUTO (dashed) and the valve % (right axis, shaded) over 1, 2, 5, 10 or 30 min or 1 hour, chosen with the Up and Down buttons beside the Range card. The pressure axis is at least 25 % of the highest pressure or setpoint shown tall (1.5 mbar at least), so a steady hold looks steady: 1.5 mbar at 2 and 5 mbar, 2.5 mbar at 10 mbar. The valve axis covers the valve's range over the window, at least 20 % tall. The Home sparkline is at least 10 mbar tall, so it shows only a big change |
| Events | Menu > Events | One list, newest first. The latched faults are on top in red with a chevron; a tap opens the fault detail: Cause, Controller (what it did), Check and Reset when, with Next fault, Reset fault and Close. Under them the event log, the last 100 events, each the time and tag over the text. Reset fault (or Reset faults) is in the side column. With no fault: "No faults latched" |
| PID | Menu > PID, or the PID tile on Home | Tabs Gains, Zones and Settings (section 10). Gains: the Zone 1 to Zone 4 track, the zone shown raised and the zone in use underlined (Zone 4 greyed with three zones), over that zone's KP, TI and KD cards. Zones: a bar of the zones over the pressure, and the Zone 2, Zone 3 and Zone 4 cards (the pressure where each starts; None for no Zone 4). Settings: AUTO min, SP band, Hysteresis and Deadband |
| Valve | Menu > Valve | Tabs Position, Homing and Limit (section 7). Position: Position %, Travel (turns), Homed (when the last homing was; No after Set as %; Before Off when kept from Shut down) or Last saved %, and the Set as % and Home valve buttons; the strip reads "X turns. Valve idle". Homing: Fast close, Slow close, Back-off and Return. Limit: the Disable range bar, From %, To % and the Disable switch toggle; the strip reads "Switch released" or "Switch pressed", with the disabled range while it is On |
| Motor | Menu > Motor | Tabs Jog and Settings, with the valve % beside them ("----" until homed or set, "Homing" while homing). Jog: Close, Limit override and Open (section 6). Settings: Speed, Start, Accel, Jog, Steps/turn and Direction (section 10); the strip gives the full-travel time at Speed |
| Pressure sensor | Menu > Pressure sensor | Row 1: Pressure (mB, by the calibration in use), Signal (mV/V) and Calibration (Nominal, Saved or Not saved). Rows 2 and 3: the Vacuum point and Air point cards, each with Capture and what it holds; a point captured since the last save has a dark edge on the left (section 8) |
| System | Menu > System, or the IP tile on Home | IP address and mask (a tap opens its page), MAC address, Date and time, Network control (On: can set, or Off: read only; a tap on the card switches it), Firmware (version and build date), Up time and Power (the Power options question). The strip shows a new address waiting for the restart |
| Diagnostics | Menu > Diagnostics | Live raw readings on three row cards. Sensor: Supply (the excitation, V), Output (the Druck output, mV) and Pressure (before calibration, by the nominal line, mB; near Barometer in air). Motor: Position (steps), Position (turns) and Limit switch (Pressed or Released, read 50 times a second). Board: Barometer (the Pressure 21 Click, mB), Temperature (the Power Monitor Click's die, °C) and Network (No cable, No client or Connected). ---- means no reading: Supply, Output, Pressure and Temperature all read ---- while the Power Monitor Click does not answer |

Display formats: pressure to 1 decimal in mB, valve position and keypad entries to 2 decimals in %, dates DD/MM/YY.

Keypads: a tap on a value box opens a number pad as a popup, with the title and the present value on the left and the entry in a blue-edged field on the right. Set applies the value. A value outside the range is refused with "Out of range" and the range; the next figure typed starts a new entry. Cancel, or a tap on the dimmed page beside the pad, leaves the old value.

Short messages: a refused action puts its reason in the strip for a few seconds, for example "Stop AUTO first", "Home the valve first", "Reset the fault first", "Limit switch pressed: hold Limit override to close" or "Wait for the valve to stop". An action that worked may say so the same way ("Position set. Not verified by homing.", "All settings saved 17/11/26 10:02").

## 6. MANUAL and AUTO

### MANUAL

In MANUAL the operator positions the valve and watches the pressure. Position is in percent of full travel: 0 % is just above the closed valve, 100 % is 4 output turns open by default.

- Tap the Go to box on Home to type a position, then Set. 0 and 100 close and open the valve fully.
- Moves run at the motor's Speed (Menu > Motor > Settings). A new Go to during a move retargets it: further on extends the move, behind it ramps down and turns round.
- The limit switch stops any closing move. If it trips away from 0 %, or reads pressed with the valve away from the closed end, the controller asks Skip or Set as closed (W-1, section 9).
- Go to needs the valve homed or its position set. Before that a tap says why and nothing moves.

Jog (Menu > Motor > Jog) moves the valve while Close or Open is held, at the Jog speed (100 steps/s by default, slower than Speed so a short press nudges the valve), and ramps down on release. It works without a position reference. Open stops at 100 % unless Limit override is held. Close stops at the limit switch, except with Limit override (section 2). Jog is refused in AUTO. The strip shows the step count and the motor state (Opening, Closing, Closing past the switch, Valve idle).

### Starting AUTO

AUTO needs the valve homed (or its position set), no fault latched, a valid pressure reading and the limit switch released. Press Start AUTO in the Pressure tile on Home.

- AUTO starts at once at the pressure shown, as the setpoint, with no question ("AUTO started at X mB, the pressure now"). The valve starts from where it is.
- For another setpoint, tap the SP box that then takes Start AUTO's place and type it; Set applies it at once, with no question, however large the change.
- Start AUTO is grey until the valve is homed or set. A refused start says why in the strip: Home the valve first, Reset the fault first, No pressure signal, Limit switch pressed: open the valve until it releases, the limit switch question waiting, a Jog button held, or a calibration point being taken.

The entry is bumpless: the loop starts from where the valve is, so the first move is small.

### In AUTO

Once a second, after the pressure reading, the controller works out a new valve position and moves to it. The strip reads Regulating to X mB, then In band, X mB when the pressure is within the setpoint band (0.10 mbar by default; with a band of 0 it stays Regulating), then Stable at X mB after 60 s in the band. The Trend page shows the setpoint as a dashed line.

- KP acts on the pressure, not on the error, so a setpoint change does not move the valve at once: the integral takes it to the new setpoint, and the pressure comes in from one side without a bounce. KD (0, off, by default) acts on the pressure's rate of change.
- The valve moves in short bursts rather than continuously. A command inside the Deadband (8 steps by default) is held until it adds up to it, then made in one go, so a needle that sticks when opening is moved past its stick at once. A reversal is held until the command is 0.2 mbar of error at the zone's KP from the valve, and never more than 0.1 % of travel, so the reading's noise does not turn the motor round inside the drive's backlash.
- AUTO keeps the valve at AUTO min or above (0 % by default). With the valve held there and the pressure above the setpoint the strip reads "Valve held at AUTO min X %". With the valve fully open or closed for 60 s and the setpoint not reached it reads "Valve at 100 %: X mB not reached" (or 0 %).
- The PID table picks one of four PID zones by the lower of the pressure and the setpoint (section 10). The PID tile on Home shows the KP, TI and KD of the zone in use, and PID > Gains underlines that zone. A change of zone does not move the valve and is logged in Events ("AUTO PID zone 3 to 2, pressure 16.9 mbar").
- In AUTO the Valve tile has Stop AUTO in place of the Go to box. Homing, Set as %, Jog and changes to the motor and valve settings are refused with "Stop AUTO first". The PID values and the Disable switch edges can be changed and saved in AUTO; they are used from the next control step.
- The controller watches for a pressure that does not respond to valve moves (W-2). This ends AUTO; section 9 says what to do. AUTO does not end for being off the setpoint, however long: during a cool-down, or with a setpoint the cryostat cannot reach, the strip reads Regulating and the loop keeps working.

### Leaving AUTO

- Stop AUTO in the Valve tile on Home goes to MANUAL at once, with no question. The valve ramps down if it was moving and holds where it stops. The setpoint is kept but not used: the next Start AUTO takes the pressure shown.
- STOP does the same.
- A fault, W-2, or W-1 also ends AUTO. The valve stops and the strip says why.
- Loss of the motor 12 V supply is not detected directly. AUTO and the step count can continue while the motor is stationary. W-2 may later detect a lack of pressure response, but it is not a motor-supply detector.

When AUTO ends for any reason the valve stays where it was. It does not close. If the cryostat needs the valve elsewhere, move it in MANUAL.

## 7. Homing and the valve position

Homing finds the closed limit switch and sets 0 % from it. It is the only way the controller checks the valve position. Home after every power-up that does not keep the position from Shut down, after a Direction change, and whenever the position is in doubt.

Start it from Home valve in the Valve tile on Home (while NOT HOMED; it asks whether to home or set the position) or from Menu > Valve > Position. The question "Home the valve? It closes onto the limit switch to find 0 %, up to N min." confirms it. The strip shows the stage; Menu > Diagnostics shows the step count and the live limit switch.

1. If the switch is already pressed, the valve moves off it first (800 steps). If the switch stays pressed, fault F-3.
2. Fast close to the switch at the Fast close speed (400 steps/s). If the switch is not found within 1.1 times the full travel, fault F-2.
3. Back off the switch by Back-off (400 steps).
4. Slow close onto the switch at the Slow close speed (25 steps/s). This point is the closed valve.
5. Open by Return (400 steps), then check that the switch has released. This last point is 0 %. Return and Back-off are independent settings on Menu > Valve > Homing. The closed valve reads `-100 × Return / full-travel steps` %, about -0.6 % with the defaults. A switch still pressed after the first move off it, Back-off or Return raises F-3.

The strip reads "Homing: moving off the switch" for step 1, then "Homing 1 of 4: fast close, up to N min", "Homing 2 of 4: backing off", "Homing 3 of 4: slow close" and "Homing 4 of 4: return", and at the end "Homed in N s" (with where the switch was against the last homing). STOP cancels homing and keeps the earlier homing, if there was one. Valve > Position then reads Homed with the date and time.

### Setting the position without homing

Set as % takes the position from the operator without moving the valve. It is on Valve > Position, on the start-up question, and on the question behind Home valve while NOT HOMED. A keypad "Set position as, %" takes 0 to 100 %, and shows the last saved position when there is one: type 0 when the valve is where 0 % should be, or the last saved value to use it. The note "Position set. Not verified by homing." follows. The start-up question's Use X % does the same with the saved value in one tap.

Neither is checked against the limit switch. Valve > Position reads Homed: No afterwards, Home's strip reads "Position unverified" at rest, and the Power options question warns that 0 % may not be the closed valve. Home the valve when you can.

### Limit switch during a move

If the switch trips during a closing move away from 0 %, the valve stops and a question asks, over any screen: "Limit switch pressed at X %. Skip: keep the current position. Set as closed: this becomes 0 %." (W-1). The count is never zeroed on its own.

- Set as closed when the valve really is closed, for example after a direction change or a slipped drive. The switch point becomes 0 % and the note "Closed position set." follows.
- Skip when the switch tripped early (ice, a stiff drive). The count is kept. Open the valve, let the ice clear, and home again later. Skip does not ask again until the switch has released.

A switch that reads pressed for 0.2 s with the valve away from the closed end, moving or not, asks the same question. In AUTO it first ends AUTO, and the question opens with "AUTO stopped: limit switch pressed".

### Disable switch

Some valves bind part way and trip the force-actuated switch with the needle well off the seat. Menu > Valve > Limit has a Disable switch toggle for that: while it is On and the valve is homed, the limit switch neither stops closing nor raises W-1 or F-3 between the From and To openings (% open, 5 and 50 by default; From 0 to 99, To 1 to 100, From below To). Below From and above To the switch works as always. With From 0 closing stops only at 0 % by the step count.

A press anywhere on the Disable switch card turns it On or Off at once. It is Off after every restart. From and To are saved settings. While it is On the bar on the Limit tab shows the range as "Disabled", and Home's valve bar shows it in yellow.

## 8. Sensor calibration

The Druck output is ratiometric, so the controller reads the ratio of signal to excitation in mV/V and turns it into pressure with a two-point calibration: a vacuum point and an air point. Both points are read live through the board as wired, so the board's own loading, gain and offset sit inside the calibration. With no calibration saved the nominal line (0 to 10 mV/V for 0 to 1034.2 mbar) is used, and Menu > Pressure sensor reads Calibration: Nominal. Readings were about 5 % low on the bench with the nominal line.

Calibrate when a sensor is first fitted to a board, when the sensor or the Click boards are changed, and when a check against the reference gauge shows drift. Calibration is refused in AUTO.

On Menu > Pressure sensor:

1. Pump the sensor down. Tap Capture on the Vacuum point card. The question "Is the sensor at vacuum? Capture sets this point to 0 mB." has Capture, which takes the point as 0 mbar, and Enter value, which opens a keypad (0 to 50 mbar) for the reference gauge reading: type what the Pfeiffer gauge shows. The capture averages 5 readings over about 5 s; the card reads "Capturing 1 of 5" and so on, then the pressure and signal it took.
2. Vent the sensor to the room. Tap Capture on the Air point card. With the Pressure 21 Click reading, the question "Is the sensor vented to room air? Capture sets this point to X mB." gives its reading: Capture takes the air point at the room pressure averaged over the same 5 readings, and Enter value opens a keypad (500 to 1100 mbar) for a laboratory barometer reading instead. Without a room reading the keypad opens straight away.
3. Once both points are held the new calibration is in use, and the strip reads "New calibration in use, not saved". Compare the Pressure reading against the reference.
4. Press Save. Undo asks, then puts the saved calibration back. With only one point captured, the strip says the calibration is not in use until the other is captured, and Undo clears that point. A point captured on its own is kept through other settings changes and saves until the second is captured.

Either point can be redone on its own; the other is kept. A capture is refused when the sensor is plainly not at the point ("not at vacuum, pump the sensor down first", "not at room air, vent the sensor first"), and is cancelled if the reading is lost ("Capture cancelled: no pressure signal", or "Capture cancelled: no barometer reading" for an air point taken at the room reading) or STOP is pressed, on the screen or over the network. The calibration is taken on the screen only; the network cannot change it.

The board holds one sensor, the 15 psia Druck, with one calibration. A replacement sensor is calibrated in place: capture the two points again and save.

## 9. Faults and warnings

Faults latch until Reset fault (on the strip, in the side column of Events, or in the fault detail), which clears only those whose cause has gone. A Reset that leaves one says so ("F-1 not reset: cause still present"). The strip shows the highest-priority fault; the number order is the priority order. Any fault in AUTO stops the valve and switches to MANUAL. F-1 is the sensor fault and does not block valve moves, so the valve can still be moved and homed with no pressure reading. F-2 and F-3 stop the valve, leave it not homed and block moves until reset. Tap a fault on Events for its detail: Cause, Controller (what it did), Check and Reset when.

| Fault | Cause | What the controller did | Check | Reset |
| --- | --- | --- | --- | --- |
| F-1 No pressure signal | No pressure sensor reading: the Power Monitor Click not answering, the ratio outside 0.005 to 11 mV/V, or the sensor supply under 7 V or tripped. The event names which | Pressure shows dashes. AUTO stops. The valve remains in position and can still be moved | The pressure sensor cable and connector, the Power Monitor Click, the Boost 10 Click and the board's 5 V. A supply trip clears only after the power is switched off and on | After the reading returns |
| F-2 Limit switch not found | During homing the valve closed 1.1 times full travel without reaching the limit switch | Stopped. The valve is not homed | Before homing again: the limit switch and its wiring, and that the valve turns | Any time. Homing again closes up to 1.1 times full travel further |
| F-3 Limit switch stays pressed | The limit switch stayed pressed after the valve moved off it, or was pressed during an opening move | Stopped. The valve is not homed | That the switch moves freely (ice) and its wiring is intact. On an opening move, check Direction (Motor > Settings): the valve was in fact closing. After homing: Back-off and Return on Valve > Homing | Any time. Then home the valve |

Warnings do not latch. W-1 clears when its question is answered. W-2 shows on the strip until OK is pressed or the next Start AUTO.

| Warning | Cause | What the controller did | What to do |
| --- | --- | --- | --- |
| W-1 Limit switch pressed at X % | The switch tripped during a closing move away from 0 %, or read pressed for 0.2 s with the valve away from the closed end, moving or not | Stopped the valve and asks Skip or Set as closed, over any screen. In AUTO: AUTO ended, MANUAL, valve idle | Set as closed if the valve is closed. Skip if the switch tripped early (ice) or its wire is open, then check the switch and its wiring. Section 7 |
| W-2 No pressure response | In AUTO the valve moved but the pressure did not answer (less than 0.05 mbar) over 5 windows of 30 s in a row. A window in which the valve moved less than 1000 steps (or the Deadband, if larger) is not judged and starts the count again | AUTO ended, MANUAL, valve idle where it stopped. Nothing latches | Needle blocked (ice), helium supply lost, drive slipping, motor unplugged or unpowered. A setpoint below what the pump can reach can raise it too. Press OK. Start AUTO again when ready |

Other strip lines:

- **Limit override held: press Close (or Close or Open)**, **Closing past the switch**: the override on Motor > Jog is in use. Release either finger to stop a closing override.
- **Valve not homed**: the valve position is not known.
- **Position unverified**: the position was set with Set as % or taken from the saved position, not homed.
- **On the switch**, **Limit switch pressed at X %**: the switch reads pressed with the valve at rest.
- **Valve above 100 %**: the count is past full travel (after a Travel change, or a Jog with Limit override).
- **Shut down stopped: limit switch pressed before 0 %**: the limit switch tripped while Shut down moved the valve to 0 % (section 4). The limit switch question follows.

The event log (Menu > Events, or `EVENT?` over the text port) keeps the last 100 events with a time stamp: faults, warnings, mode changes, setpoint changes, homing, settings changes and saves, calibration captures, Limit override use, and sets taken over the network with the client's IP address. Each line carries a tag (for example MODE, SP, REF, CFG, CAL, LIM, NET, STOP, OFF, SYS, F-1, W-2). It is in RAM and empties at a restart.

## 10. Settings

Most settings are set once, when the controller is fitted to a cryostat; the PID gains can be changed whenever the control needs it. The settings are on four pages from the Menu (PID, Valve, Motor, Pressure sensor), each with Help, Save and Undo in its side column, and System.

- **An edit is in use at once.** The keypad's Set checks the value and puts it in use straight away, with a CFG line in Events ("Zone 2 KP set to 0.10 %/mB (screen), in use, not saved"). A card whose value is in use but not saved has a dark edge on its left, and the header shows the yellow save icon.
- **Save** writes everything in use to the serial flash in one go ("All settings saved 17/11/26 10:02"). The strip on a settings page says how things stand: "N unsaved changes", "All settings saved <date>" or "Factory settings, nothing saved yet".
- **Undo** asks ("Undo N unsaved changes?"), then puts the last save back in use.
- **What can change when.** The PID values (the gains, the zones, Hysteresis, AUTO min, SP band, Deadband) and the Disable switch From and To can be changed, saved and undone in AUTO and while the valve moves; the loop uses them from its next step. The motor and valve settings need MANUAL with the valve still ("Stop AUTO first", "Wait for the valve to stop").
- **Factory reset** (Menu) asks "Reset motor, valve and PID? Calibration and IP are retained.", then puts the factory motor, valve and PID settings in use and saves them. The calibration in use, the IP address and Network control stay. It has no undo and is refused in AUTO or while the valve moves.
- The IP address and Network control are saved as they are set. Over the network, `PIDS SET`, `TABLE SET` and the Modbus PID registers save in the same way as Save, and so also save any other setting in use and not yet saved.

The board keeps the last two saves, in two flash sectors in turn, and reads a save back before it counts; the older one is only used if the newer cannot be read. A settings record in a layout this firmware does not know is not read: the controller starts on the factory settings and says so in Events.

| Page and tab | Settings, factory values and ranges | Notes |
| --- | --- | --- |
| PID > Gains | For each of Zone 1 to Zone 4: KP, gain (% open per mbar, 0.01 to 100); TI, integral time (min, 0 to 100; 0 is Off); KD, derivative (% open per mbar/s of pressure change, 0 to 100; 0 is off). Factory: zone 1 KP 0.06 TI 0.60, zone 2 0.12/0.82, zone 3 0.80/0.94, zone 4 0.40/0.25, KD 0 in every zone (from step tests on the bench cryostat) | Change them when AUTO is too slow or swings at a setpoint. KP acts on the pressure, not the error: a setpoint change does not move the valve at once. KD acts on the pressure's rate of change. Tap a zone on the track to show its gains |
| PID > Zones | Zone 2, Zone 3 and Zone 4: the pressure where each starts (mbar, 0.1 to 1000; factory 6.0, 10.0 and 14.0). Zone 4 at 0 (None) gives three zones | Gain scheduling on the pressure: the loop gain of the cryostat changes with the pressure, so one set of gains cannot suit 1 and 25 mbar. Zone 1 is below Zone 2's start, zone 4 above Zone 4's. The zone follows the lower of the pressure and the setpoint: going up the pressure decides; going down the setpoint's zone takes over as soon as the setpoint is changed. Each start must be above the one before by more than the hysteresis either side; a refused value gives the limit ("Must be 7.3 mB or less: Zone 3 starts at 10.0 mB (15 % hysteresis)."). For one set of gains at every pressure, give all four zones the same gains |
| PID > Settings | AUTO min (% open, 0 to 50, factory 0). SP band (mbar, 0 to 10, factory 0.10; 0 is None). Hysteresis (% of a zone's start, 0 to 40, factory 15). Deadband (steps, 0 to 2000, factory 8) | AUTO keeps the valve at AUTO min or above. With SP band 0 the strip never reads In band or Stable. Hysteresis: a zone changes only once the pressure is past the start by this much (with 6, 10, 14 mbar and 15 %: up at 6.9, 11.5 and 16.1 mbar, down at 5.1, 8.5 and 11.9 mbar), so noise at a boundary cannot flip it. Deadband: AUTO holds a move, opening or closing, until it adds up to this many steps, then makes it in one go |
| Valve > Position | Travel (turns for 100 %, factory 4.00, 66048 steps) | Changing it moves the % under the valve (a note says so). Set as % and Home valve are here (section 7) |
| Valve > Homing | Fast close and Slow close (steps/s, factory 400 and 25, 16 to 1000). Back-off and Return (steps, factory 400 each, 10 to 5000) | Back-off follows the fast close; Return follows the slow close and defines 0 % |
| Valve > Limit | From and To (% open, factory 5 and 50; From 0 to 99, To 1 to 100, From below To), and the Disable switch toggle | From and To are saved settings; the toggle is not saved and is Off after every restart (section 7) |
| Motor > Settings | Speed (steps/s, factory 400, 16 to 1000): every move but Jog and the homing closes. Start (steps/s, factory 200, not above Speed): where a ramp starts and ends. Accel (steps/s², factory 3200, 10 to 20000). Jog (steps/s, factory 100): Close and Open on Motor > Jog. Steps/turn (factory 16512, 48 steps × 344:1): the turns shown, not the travel. Direction (Normal or Reverse) | Saved when the valve is stopped, in MANUAL. Changing Direction means the valve must be homed again: until it is, Set as % and the saved position are refused ("Direction or coil order changed: home the valve first"). Steps/turn changes only the turns shown. The strip gives the full-travel time at Speed |
| Pressure sensor | The two-point calibration (section 8) | In use once both points are held; Save keeps it |
| System | IP address and mask (its page: Address, Mask, Save, Default). Network control (On or Off, factory On) | Saved as they are set. A new address is used after a restart, and the strip says so until then. Default fills in the build's address, 192.168.1.100 with mask 255.255.255.0, for Save. The address cannot be changed over the network |

Factory values for the valve and motor are for the PK22 344:1 stepper on the Oxford Instruments needle valve. The factory PID gains are from the bench cryostat; the operator sets the gains for another cryostat on Menu > PID.

## 11. Remote access

The controller runs two servers on one cable and one IP address: the text port on TCP 8888 and Modbus TCP on port 502. The USB-C port is for power and flashing only.

### Network and IP address

The controller has a fixed (static) IP address: the one saved on Menu > System, or the build's default 192.168.1.100 with mask 255.255.255.0. There is no gateway, so it answers only on its own subnet, and there is no DHCP or DNS, so connect by the IP address. Home shows the address in use as address/prefix (192.168.1.100/24); Menu > System shows it with the MAC address.

To set the address, tap IP address and mask on System, type the address and the mask, press Save and restart. The address cannot be changed over the network, so a remote user cannot cut off their own link. Put the PC on the same subnet with a fixed address: for the default, for example 192.168.1.10 with mask 255.255.255.0. On a site network, give each controller its own address on the instrument subnet: they all start at 192.168.1.100.

Menu > Diagnostics > Network reads No cable while the Ethernet link is down, No client with the cable in and nobody connected, and Connected while a client is connected to either server. Pulling the cable drops both servers' clients, so they reconnect when it is back. A client lost behind a switch (the PC switched off, the cable pulled at the far end) is not detected; a new connection replaces it.

### Network control

Menu > System > Network control decides whether the network may change anything. On (the factory setting): set commands and Modbus writes are taken. Off: the network can only read; every set command answers ERR 211 and every Modbus write exception 1, STOP included. The switch is saved as it is set. A set taken over the network is logged as a NET event and shows on the strip for 5 s ("Network: setpoint 7.5 mbar"); reads and refusals show nothing on the screen.

### Port 8888: text commands

Port 8888 takes the remote command set in plain text, one command per line (upper case, ending in LF or CR LF, at most 96 characters), for a supervisory program or a person at a terminal (PuTTY in Raw mode). One client at a time; a new connection replaces the old. There is no echo and nothing is sent unasked: faults, warnings and switch changes go to the event log. A query (ending in ?) answers one line that starts with its own words. A set command answers nothing when it is accepted, and ERR with a code and a reason when it is refused. Several commands can share a line, separated by ;, and their query replies come back on one line.

| Group | Commands |
| --- | --- |
| Reading | `PRESSURE?` the pressure (mbar, 3 dp). `SETPOINT?` the setpoint. `MODE?` MANUAL, AUTO, HOMING, NOT HOMED, SHUTDOWN or OFF. `VALVE?` the position (%, 2 dp); `VALVE MOVING?` 1 while moving or homing. `ALARM?` the latched faults and the warnings. `EVENT?` followed by a number, the last events (on a line of its own). `BAROMETER?` the room pressure. `TIME?` the clock. `PIDS?` the zone in use, then KP, TI, KD of zones 1 to 4 (13 numbers). `TABLE?` the PID table: where zones 2, 3 and 4 start in mbar, and the hysteresis in %. `*IDN?` the identity and firmware version |
| Control | `SETPOINT` followed by the setpoint in mbar (does not start AUTO). `MODE AUTO` starts AUTO with the same checks as the screen; `MODE MANUAL` ends it, the valve stays where it is. `VALVE SET` followed by a % moves the valve in MANUAL. `STOP` stops the valve and ends AUTO, homing, a shut down or a capture; it is the only stop. `ALARM RESET` clears the faults whose cause has gone; `ALARM OK` acknowledges W-2. `TIME SET 2026-10-27 09:00:00` sets the clock. `PIDS SET 2,0.8,0.3` sets and saves KP and TI (a fourth number sets KD) of zone 2 (or 1, 3 or 4), as Save on the PID page; it is taken in AUTO at the next control step. `TABLE SET 6,10,14,15` sets and saves where zones 2, 3 and 4 start (mbar; the third 0 for three zones) and the hysteresis (%). Each change is a save in flash: send it only when the values change |

Homing, calibration, the other settings, the motor and turning on or off are on the touchscreen only. In AUTO, `VALVE SET` answers "Switch to MANUAL first". While OFF or shutting down, `VALVE SET`, `MODE AUTO` and `MODE MANUAL` answer ERR 213. With Network control On, any command not in the table answers ERR 100; with it Off, queries still work and every other line answers ERR 211. The error codes and the full table are in `docs/text-port-commands.md`.

### Port 502: Modbus TCP

The Modbus server is for a supervisory system. It reads 59 holding registers (0 to 58, map version 4) and 14 coils, and writes only the setpoint, the valve position in MANUAL, the PID zones and table (registers 27 to 58), and the AUTO, STOP, RESET and warning OK coils. Homing, the switch question, calibration and the other settings are not on Modbus, because each needs a person watching or changes what the controller trusts.

- Function codes 1, 3, 5 and 16. Up to 2 clients at once. Unit id not checked. Addresses are 0-based.
- Floats are IEEE 754 single precision in two registers, high word first. NaN means no value.
- Values update once a second, so polling faster than 1 Hz adds nothing. Read registers 0 to 58 in one request.
- Register 1 is a heartbeat (seconds since start-up). A value that stops changing means the controller has stopped.

| Address | Content |
| --- | --- |
| Register 2 | Mode: 0 NOT HOMED, 1 HOMING, 2 MANUAL, 3 AUTO, 4 SHUTTING DOWN, 5 OFF |
| Register 3 | Status bits: reading valid, position set, homed, moving, switch pressed, fault, warning, setpoint set, writes on, one reserved bit (always 1) |
| Register 4 | Regulation in AUTO: 1 Regulating, 2 In band, 3 Stable |
| Registers 5, 6 | Faults latched (bit 0 F-1, bit 1 F-2, bit 2 F-3) and warnings showing (bit 0 W-1, bit 1 W-2) |
| Registers 10 to 11 | Pressure, mbar |
| Registers 12 to 13 | Setpoint, mbar (writable, any mode) |
| Registers 14 to 15 | Valve position, % |
| Registers 16 to 17 | Valve going to, % (writable, MANUAL only) |
| Registers 20 to 23 | Excitation V and controller temperature °C |
| Registers 24 to 25 | Up time, s |
| Register 26 | PID zone in use, 1 to 4 |
| Registers 27 to 32 | PID table: 2 (by the pressure, the only choice), where zones 2 and 3 start (mbar, floats), hysteresis % (writable, any mode, saved) |
| Registers 33 to 56 | KP, TI, KD of zones 1 to 4, floats (writable, any mode, saved) |
| Registers 57 to 58 | Where zone 4 starts, mbar (0 for three zones; writable, saved) |
| Coils 0 to 3 | AUTO (1 starts, 0 switches to MANUAL), STOP (as the STOP button: it also ends homing, a shut down or a capture), RESET, warning OK |

A write the controller refuses answers exception 4 and logs the reason as a NET event, with the client's IP address. Exception 1 means the function code is not supported or writes are off; 2 an address outside the map or a read-only register; 3 a bad value or count. The complete map with every bit is `docs/modbus-register-map.md`.

If the supervisory system stops polling, the controller carries on as it is: it does not change mode or move the valve.

## 12. Maintenance

| Task | When | How |
| --- | --- | --- |
| Check the pressure reading against the reference gauge | At each cool-down, and whenever the reading looks wrong | Compare the controller's reading with the Pfeiffer gauge at vacuum and at air. Recalibrate (section 8) if either point is off by more than the sensor's accuracy |
| Home the valve | After every power-up that does not keep the position from Shut down, after a Direction change, and when the position is in doubt | Section 7 |
| Check the limit switch | When F-2, F-3 or W-1 appears, and at each warm-up | Diagnostics shows Pressed or Released live, as does the strip on Valve > Limit. Press the switch by hand and watch it change. Look for ice on the switch and the valve drive |
| Check the Druck excitation | When F-1 appears and its event names the excitation | Diagnostics shows the Supply. It should be about 10 V; under 7 V there is no reading. A Boost 10 Click trip needs a power cycle |
| Check the motor supply | When moves are slow or the motor is noisy | Measure the 12 V at the Stepper 3 Click. The controller does not measure it |
| Review the event log | After a fault, and before clearing it | Menu > Events, or `EVENT? 100` over the text port. The log empties at a restart, so read it first |
| Save the settings | After any settings change | Save on the settings page, or the save icon in the header. The strip on a settings page shows the date of the last save |
| Set the clock | When System > Date and time reads Not set or wrong | `TIME SET` over the text port, in the form `TIME SET 2026-10-27 09:00:00` |
| Update the firmware | When a new version is released | Flash over USB-C with mikroBootloader. The settings, calibration and saved position are in serial flash and survive reflashing. Check Menu > System, or `*IDN?` over the text port, afterwards |
| Clean the touchscreen | As needed | Shut down first, and switch off, or the taps will act on the screen (a double tap on the OFF screen turns it on). A soft cloth, no solvents |

The controller has no fuse, battery or filter to replace. The real-time clock runs from the board supply; a cell in the BAT1 holder keeps it through a power-off.

If the controller stops responding, the independent watchdog restarts it within about 4 s. Every restart comes back OFF, so the controller stays off until someone double taps the screen; AUTO does not resume by itself. The valve does not move after the restart until it is homed or its position is set, and a position saved before the reset is offered.
