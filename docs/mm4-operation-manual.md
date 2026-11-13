# mm4 Operation Manual

Revision BD, 13 November 2026. Firmware 0.16.27. Revision C checked the instructions against `main` commit `bbf2ec46fa6dccabae46dadd32ce076985a15677` (firmware 0.14.8); revision D adds the static IP address (0.14.9 to 0.14.11) and renames the Faults and log screen to Events (0.14.12); revision E covers the Home PID tile, the Maintain button and the PID tuning page, the removal of the Valve page (0.14.21) and of the setpoint change question (0.14.22), and the mode shown as MANUAL mode or AUTO mode (0.14.23); revision F adds Shut down and the OFF state (0.15.0); revision G covers Shut down to 0 %, every start in OFF and the power icon at the top left (0.15.1); revision H covers the mode panel in the header (0.15.2); revision I covers Home without the mode button (0.15.3); revision J the dark blue AUTO panel and the faster Start AUTO / Stop AUTO (0.15.4); revision K the wider mode panel (0.15.5); revision L the IP tile on Home (0.15.6); revision M adds the choice to close the valve or not on the Shut down question, and the start-up with no white flash (0.15.7); revision N adds the PID? and PID SET text port commands (0.15.8); revision O covers the common screen template: header, Menu, Settings, System, keypad popup, and the IP tile as address/prefix (0.15.9); revision P left-aligns the IP tile (0.15.10); revision Q removes W-6 (0.15.11); revision R centres the IP tile again (0.15.12); revision S lines the mode panel up with the third column's right edge (0.15.13); revision T adds the remote light to the header (0.15.14); revision U adds gain scheduling (three PID sets), the opening and closing deadbands, and their text port commands and Modbus registers (0.15.15); revision V closes a popup on a tap on the greyed area round it (0.15.16); revision W holds the remote light steady while a client polls (0.15.17); revision X notes the shorter on-screen Help (0.15.18); revision Y puts Calibrate sensor on the Menu and Diagnostics on System (0.15.19); revision Z renames the gain schedule PID table, removes the setpoint table, shows only KP, TI and KD on the Home tile and lines up the set-in-use dot (0.15.20); revision AA names the Menu tile Calibration (0.15.21); revision AB drops the DHCP wording and moves Review and save to the second row of Settings (0.15.22); revision AC puts Settings on the right of the Menu's Setup row and fills it and Review and save dark blue (0.15.23); revision AD makes the OFF screen's power symbol and lettering brighter (0.16.0); revision AE adds the yellow Limit tag on the Valve tile while the limit switch is pressed (0.16.1); revision AF puts every page on the Menu as nine icon tiles without row captions, removes the Settings page and Choose sensor (one sensor per board) and renames Valve setup Valve settings (0.16.2); revision AG swaps Valve position and PID tuning on the Menu (0.16.3); revision AH shows the pressed limit switch as a yellow edge round the Valve tile and raises W-4 after 0.2 s, the strip following at once (0.16.4); revision AI adds Motor override > Switch > Pass 10 to 20 % (0.16.5); revision AJ renames the Menu tile Calibration to Pressure setup (0.16.7); revision AK widens the switch pass band to 5 to 25 % (0.16.8); revision AL scales the reversal hold with KP and picks the PID set from the lower of the pressure and the setpoint (0.16.9); revision AN widens the switch pass band to 5 to 50 % (0.16.11); revision AO makes the text port command names whole words (0.16.12); revision AP keeps the setpoint change's proportional step when it also changes the PID set (0.16.13); revision AQ adds a fourth PID set and boundary 3 (0.16.14); revision AR makes KP act on the pressure only, so a setpoint change gives no kick (0.16.15); revision AS lets the setpoint band go down to 0 and renames Valve settings Motor settings (0.16.16); revision AT moves Motor override from Valve position to a fourth tab on Motor settings, Override (0.16.17); revision AU makes the bench PID table the factory settings and gives Trend and the Home sparkline a least pressure span (0.16.18); revision AV moves the switch pass band from Motor override's Switch tab to a toggle on Motor settings > Override (0.16.19); revision AW keeps the PID table always on and removes its Off choice (0.16.20); revision AX renames the switch pass Ignore switch with clearer wording, makes its edges saved settings and puts Drive first on Motor settings (0.16.21); revision AY renames it Disable switch, lets its edges go from 0 to 100 % and shortens the help pages and card lines (0.16.22); revision AZ widens the least pressure span on Trend and the Home sparkline to 25 % of the pressure, 1.5 mbar at least (0.16.23); revision BA rearranges System: Cryostat name and Remote control, IP address and MAC, Firmware with Restart and Shut down, and no Diagnostics button (0.16.24); revision BB reworks PID tuning after a usability review: tabs Gains, Set ranges and AUTO, the set row as cards, boundaries captioned by the sets they split (0.16.25); revision BC gives the Home sparkline a least span of 10 mbar and makes the header's remote arrows smaller (0.16.26); revision BD reworks Diagnostics and renames Pressure setup to Pressure sensor, its page Sensor calibration, after a usability review (0.16.27). It does not add hardware bench verification.

This manual covers day-to-day use of the mm4 pressure controller (Option C) at firmware 0.15.15: power-up, Shut down, homing, MANUAL and AUTO, calibration, faults and warnings, settings, and remote access over Ethernet. It is written for the operator at the cryostat.

| Field | Value |
| --- | --- |
| Equipment | mm4 pressure-based needle-valve controller for helium-flow cryostats (CF-10, CF-11, HM-2, AVM-1, AVM-2) |
| Hardware | MikroE Mikromedia 4 for STM32F4 with Stepper 3, Boost 10 and Power Monitor Click boards; Druck UNIK 5000 15 psia transducer; geared unipolar stepper on the needle valve |
| Firmware | 0.14.8, `main` commit `bbf2ec46fa6dccabae46dadd32ce076985a15677` |
| Sources | Current behaviour: firmware source in `InitialTesting/`, README and docs/modbus-register-map.md at the commit above. Historical design and bench references: MM4 Design Description Rev S; Control Logic Rev O; Firmware Study Guide (0.12.16); mm4 Screens and Messages (0.11.36); Port 5000 and Modbus (0.12.7); bench-test notes for stages 0 to 8, Modbus and Ethernet |
| Status | Draft, prototype firmware. Items the bench has not confirmed are marked (not benched) where they appear and listed in section 13 |
| Exclusions | Build and flash procedure, firmware structure, Option A (EPC3008 + STF06-R), the simulator, PID tuning theory |

Contents: 1 Overview, 2 Safety and limits, 3 Hardware and connections, 4 Power-up and first use, 5 Screens, 6 MANUAL and AUTO, 7 Homing the valve, 8 Sensor calibration, 9 Faults and warnings, 10 Settings, 11 Remote access, 12 Maintenance, 13 Items not yet benched, 14 Revision history.

## 1. Overview

The controller holds the pressure on the cryostat pumping line at a target the operator sets, by driving the needle valve's stepper motor. The pressure is measured by the Druck transducer on the line. Pressure is a rough proxy for the helium flow through the cryostat. The flow itself is not measured, so the readings and screens are always in mbar and never labelled as flow.

What the operator does:

- Homes the valve after power-up, so the controller knows where closed is.
- Sets the target pressure for the cryostat, from experience. Typical targets are 1 to 25 mbar.
- Switches between MANUAL (the operator positions the valve in percent open) and AUTO (the controller regulates the pressure to the target).
- Watches the trend. A steady pressure matters more than the exact value. The sample temperature is watched on another instrument.
- Clears faults after fixing their cause.
- Shuts the controller down with the power icon on Home before switching it off (section 4).

What the operator does not do: tune the control loop. KP, TI and KD are set once at commissioning (Menu > PID tuning) and are not part of normal operation. Home shows them so the operator can see what the loop is using.

The controller has six modes, shown at the right of the header on every screen, on a flat panel the header's height, from just left of the buttons below it to the screen's edge (grey, dark blue in AUTO; with a latched fault it reads FAULT, in red):

| Mode | Meaning |
| --- | --- |
| NOT HOMED | The valve position is not known. Nothing moves at power-up until the operator homes the valve or sets its position |
| HOMING | The valve is finding the closed limit switch |
| MANUAL | The operator positions the valve. The pressure is shown but not regulated |
| AUTO | The controller moves the valve once a second to hold the target pressure |
| SHUTTING DOWN | Shut down is moving the valve to 0 %. The screen is black with a bar and STOP |
| OFF | Every power-up starts here, and Shut down ends here. The motor coils are off and nothing moves. The screen is black: a double tap turns the controller on |

A red header with FAULT means a fault is latched (section 9).

The controller is operated from its 4.3 inch touchscreen. It can also be read and commanded over Ethernet: a text command link on TCP port 8888 (the remote command set) and a Modbus TCP server on port 502, for a supervisory system or a person at a terminal (section 11). The USB-C port carries the same text link when the firmware is built for USB instead of Ethernet.

## 2. Safety and limits

The controller has no hardware interlock. The closing stop is the firmware reading the valve's limit switch before every closing step. This was a design decision for the prototype. The points below follow from it.

- **The limit switch can trip early.** It is a force-actuated normally closed contact. Ice on the valve, or a stiff drive, can press it before the needle reaches the seat. The controller cannot tell an early trip from the seat. When the switch trips during a normal move the controller stops and asks the operator (W-1, section 9). Only a person at the cryostat can judge which it was.
- **STOP** on the bottom row of every screen stops the valve at once and holds it. In AUTO it also switches to MANUAL. Nothing latches, so operation can resume straight away from the screen. STOP works with any finger, even with another one on the glass.
- **Nothing moves at power-up.** Every power-up and restart starts OFF, and only a double tap on the screen turns the controller on. The network cannot turn it on, so AUTO does not resume after a power cut until someone is at the screen. The valve is NOT HOMED until the operator homes it or sets its position. The controller never uses a saved position on its own, with one exception: after Shut down, if the limit switch reads released at power-up, it keeps the position it shut down at (section 4). The valve does not move either way.
- **Shut down before switching off.** Shut down moves the valve to 0 % and records the position, so the next power-up needs no homing. Switching off without it is safe (the valve stays where it is) but the next power-up asks for the position or a homing.
- **Limit override** (Menu > Motor settings > Override > Open Motor override) drives the valve closed past an open limit switch. It takes two fingers: hold Limit override, then press Close with another finger. Holding Limit override alone does nothing. A closing override moves only while both fingers are held and stops immediately when either lifts. Open also works while Limit override is held; an opening move ramps down when either finger lifts. Use it only when the switch is known to be stuck and the valve is known to be off the seat.
- **Motor supply.** The Stepper 3 Click has no current limit. The motor supply must be a regulated 12 V. The firmware does not measure it: losing motor power does not directly end AUTO, and the counted position can keep changing while the motor is stationary. Check the supply and home again if position is in doubt. Do not feed 12 V into the Boost 10 Click, which takes 5 V from the board and makes the 10 V Druck excitation.
- **Remote control.** While Menu > System > Remote control is On (the default), anyone who can reach the controller's IP address on port 8888 or port 502 can set the target, move the valve in MANUAL, start AUTO and stop. With it Off the network can only read, and a network STOP is refused too. Keep the controller on the instrument network only.

Operating limits:

| Item | Limit | Source |
| --- | --- | --- |
| Board supply | 12 V on the terminal (10.6 to 14 V) | Mikromedia 4 |
| Druck excitation | 9.5 to 10.5 V. Outside this is fault F-2 | Firmware |
| Pressure range | 0 to 1034 mbar absolute (15 psia sensor). Display resolution 0.1 mbar; accuracy set by the Druck (±0.04 % FS, ±0.4 mbar) and the calibration points | Druck datasheet |
| Target | 0.0 to the sensor range, kept to 0.1 mbar. Operators use 1 to 25 mbar | Firmware |
| Valve travel | 0 to 100 % of 66048 steps (4 output turns) by default. 0 % is the Return point just above the seat; a seated valve reads about -0.6 % with the default Return | Firmware 0.14.8 |
| Controller temperature | W-3 at 55 °C on the Power Monitor die, clears below 50 °C | Firmware |

## 3. Hardware and connections

| Part | What it does | Connection |
| --- | --- | --- |
| Mikromedia 4 for STM32F4 | Controller and 4.3 inch 480 x 272 touchscreen. Runs the control loop, the screens, the Ethernet link and the Modbus server | 12 V terminal (2.54 mm pitch), USB-C for flashing or the USB text link, RJ45 Ethernet |
| Stepper 3 Click, socket S2 | Drives the unipolar six-lead valve stepper (PK22, 344:1 gearbox) at 12 V. No current limit | Motor cable to the Click's four coil outputs and the 12 V motor supply |
| Limit switch, socket S1 | One normally closed dry contact to ground, opens at the closed end of travel | Socket TX (PD5, internal pull-up) and GND. On the Adapter Click's 10-pin header that is pad 8, printed RX, and pad 10. The Adapter prints its UART pad names from the Click's side, so pad 8 printed RX is the socket's TX, and pad 7 printed TX is the socket's RX |
| Boost 10 Click, socket S3 | Makes about 10 V from the board's 5 V for the Druck excitation. Trips above 11.5 V. Never feed it 12 V | SPI1 |
| Power Monitor Click (INA228), socket S4 | Reads the Druck output and the excitation, 128 averages over 600 ms, once a second | I2C2, address 0x4A |
| Druck UNIK 5000 PDCR50G1 | 15 psia absolute pressure transducer, 10 mV/V, insulated from its case | M12: pin 1 +Supply to Boost 10 VOUT and INA228 VBUS; pin 2 +Out to IN+; pin 4 -Out to IN-; pin 3 -Supply to common GND |
| Pressure 21 Click (BMP581) on Adapter Click, socket S1 | Room pressure for the air point of the calibration. Driver built; fitting and bench operation were unconfirmed in the last recorded bench notes (not benched) | I2C, address 0x46 |
| On-board serial flash | Settings, calibration and the saved valve position. Survives reflashing | |
| On-board real-time clock | Time stamps for the event log and saves. Survives a reset, and a power-off with a cell fitted | |

Power: the board takes 12 V on its terminal, with USB-C and the Li-Po connector as lower-priority sources; SW1 must be on. The motor supply is a regulated 12 V from a PoE splitter. Both the board and the motor need their 12 V; the board alone runs the screens, the link and the pressure reading.

Earth: bond one 0 V point of the controller to the chassis earth. On the bench the board floating on the PoE splitter's 12 V put noise on the pressure reading. One 0 V to earth bond removed it (bench, 21 October 2026). This bond is not yet in the design drawings.

Reference gauge for calibration: a Pfeiffer TPG 202 Neo (USB-C readout only). The vacuum reading is typed into the controller by hand (section 8).

Socket pin details are in InitialTesting/clicks.h, limit.h and extflash.h, and the wiring drawings are in `docs/hardware/wiring/`.

## 4. Power-up and first use

At power-up the controller loads the newest saved settings, starts the pressure reading and shows the OFF screen: black, a light grey power symbol and "Double tap to turn on" in near-white. The valve does not move. Two taps anywhere on the screen within 0.6 s turn it on: the start-up logo and bar for about 1.5 s, then Home, in NOT HOMED unless the position was kept from Shut down.

1. Check the header. It reads NOT HOMED. Menu > System shows the firmware version and build date, the IP address and MAC address. Diagnostics (on the Menu) shows the live sensor, motor and temperature readings.
2. Check the pressure. The Pressure tile shows the line pressure in mbar. Dashes mean no valid reading (section 9, F-1). Check Menu > Pressure sensor for the calibration kind and saved state; an uncalibrated sensor uses the nominal conversion (section 8).
3. If a saved position is on offer, the screen says "Valve position unknown. Last saved position X %." and offers Use X %, Set position and Home valve. The saved position is the step count written before the last power-off. If power was lost during a move the prompt instead says the saved position is accurate to within E %. Use X % is quick but is not checked against the limit switch. Home is the checked option.
4. Home the valve (section 7). This takes up to a few minutes. The header then reads MANUAL and the valve reading is 0.00 % and the strip reads "Valve idle" once it has stopped.
5. Set the valve where the cryostat needs it, in MANUAL, or start AUTO (section 6).

If the controller was shut down with Shut down and the limit switch reads released at power-up, it skips steps 3 and 4: turned on, Home opens in MANUAL with the valve at 0.00 %, nothing is asked, and it can be moved or put in AUTO at once. The Valve position page reads "Kept at shut down" if the valve was homed before, or "Last saved, not homed" if its position had been set by hand. If the switch reads pressed (the valve was closed further while the power was off, or the switch or its wire is at fault), the position is not kept and nothing is on offer: home the valve. After switching off without Shut down, the start-up question in step 3 begins "Switched off without Shut down." instead of "Valve position unknown."

After a restart the target is not kept; Start AUTO takes the pressure shown as the target. The settings, calibration and tuning saved with Save settings are kept. The trend and the event log are in RAM and start empty.

The real-time clock keeps time through a reset. If the dates on saves and events are wrong, set the clock over the text link with the TIME SET command (section 11). There is no clock page on the screen.

### Shut down and switching off

Before switching the controller off, tap the power icon at the top left of Home (where Home is on the other screens), or Shut down on System. The question "Shut down the controller?" has a checkbox "Close the valve to 0 %", ticked each time the question opens, and Cancel, Restart and Shut down. Restart asks its own question, as on System. Tap the checkbox row to untick it: Shut down then turns the controller OFF at once with the valve left where it is (step 3 without the move), and the position is kept just the same.

1. With the box ticked, Shut down ends AUTO and moves the valve to 0 % at the normal move speed (400 steps/s by default, about 50 s from 30 % open). It does not close onto the limit switch.
2. While it moves the whole screen is black: "Shutting down", the bar emptying as the valve closes and the seconds left. STOP, in its usual place, cancels it: MANUAL, the valve where it stopped.
3. At 0 % the motor coils switch off and the controller is OFF, the same black screen as at power-up. The position is written to the serial flash at once. It is now safe to switch off.
4. While OFF the controller still reads the pressure and answers queries, but refuses moves, AUTO and homing. A double tap turns it on: the start-up logo and bar, then Home in MANUAL at 0.00 %. One tap does nothing.

If the valve is not homed, there is no 0 % to go to: the question reads "Valve not homed: shut down without moving it?", with no checkbox, and Shut down turns the controller OFF with the valve where it is. If the valve is already at 0 %, it turns OFF at once. If the limit switch trips on the way to 0 % (ice, a switch tripping early, or a count that slipped), Shut down stops in MANUAL, the strip reads "Shut down stopped: limit switch pressed before 0 %" and the limit switch question asks whether that point is the seat (W-1, section 9). With a fault latched, closing the valve is refused (reset the fault first); Shut down with the box unticked still works.

Shut down and turning on are on the screen only; the network cannot do either. The screen backlight stays on while OFF. At power-up the backlight comes on only once the OFF screen is drawn, so the panel shows no white or random pixels first.

First use of a new board or a new Druck sensor also needs: a two-point calibration of the sensor (section 8; the board holds one 15 psia sensor, so there is nothing to add or choose), the valve travel and motor direction checked under Menu > Motor settings, the pressure response direction set under Menu > PID tuning > AUTO limits, the gains (and, if used, the gain schedule) under Menu > PID tuning, and Save settings. These are commissioning steps, done once.

## 5. Screens

Every screen has the same frame: a header with Home and Back icons at the left (Home first), the title left-aligned after them, and the mode on the right on a flat panel the header's height, from just left of the buttons below it to the screen's edge (MANUAL, NOT HOMED or HOMING on grey, AUTO on dark blue, FAULT on red); left of the mode panel the remote light while a remote client is connected (section 11) and the unsaved icon while a setting is unsaved; a status strip; and STOP on the bottom row. On Home a power icon at the top left, where Home is on the other screens, opens Shut down, and there is no Back. Under a popup Home and the power icon are greyed, and Back or a tap on anything greyed (the dimmed page round the popup, the greyed strip, a greyed header icon) closes the popup as Cancel does: nothing is applied and a keypad entry is dropped. A tap on the popup's own box does not close it. Two questions need an answer and stay: the limit switch question during a move and the saved position offer at start-up. While shutting down and while OFF the whole screen is black (section 4). On Home and the operator pages the strip shows the valve's state (Valve idle, Moving to X %, or in AUTO Regulating to, In band or Stable at the setpoint) with the 5 minute pressure sparkline, and takes over for prompts, faults and messages. Under a popup the strip stays visible and greyed. Colour follows the ISA-101 convention: grey when normal, colour only for an abnormal state.

| Screen | How to reach it | What it shows and does |
| --- | --- | --- |
| Home | Home button | Pressure tile (reading in mbar, log bar 0.1 to 1000 mbar; in AUTO the SP (setpoint) box, otherwise the Start AUTO button) and Valve tile (reading in %, bar; in MANUAL the Go to box for a typed %, in AUTO the Stop AUTO button, while not homed the Home valve button; a yellow edge round the Valve tile while the limit switch is pressed, in any mode; a valve fault's red edge takes its place; the Disable switch band, From to To, 5 to 50 % by default, yellow on the valve bar while Motor settings > Override > Disable switch is on). On the right, from the top: the PID tile (the KP, TI and KD in use; with the gain schedule on, Set, KP and TI: the set in use and its gains; a tap opens PID tuning), the IP tile ("IP address" over the address and prefix the controller is using, for example 192.168.1.100/24, "USB build" on the USB build; a tap opens System), Menu and STOP. A tap on the sparkline opens Trend |
| Menu | Home > Menu | Nine tiles in three rows, each with an icon over its name and no row captions: Trend, Events, PID tuning; Motor settings, Valve position, Pressure sensor (Sensor calibration; Pressure setup to 0.16.26, Calibrate sensor); System, Diagnostics, Review and save (filled dark blue). Every page is one tap from here |
| Trend | Menu > Trend | Pressure (left axis), the target while in AUTO (dashed) and valve % (right axis) over 1, 2, 5, 10, 30 min or 1 hour. The pressure axis is at least 25 % of the highest pressure or target shown tall (1.5 mbar at least), so a steady hold looks steady: 1.5 mbar at 2 and 5 mbar, 2.5 mbar at 10 mbar. The Home sparkline is at least 10 mbar tall (5 mbar either side, from 0 mbar at low pressures), so it shows only a big change |
| Events | Menu > Events | Latched faults, with Reset fault. Tap a fault for its detail and help. The event log (last 100 events) |
| Valve position | Menu > Valve position | One page with Home valve, Set as 0 %, Set as other % and Use last saved, plus Help. Shows the position reference and output turns; during homing it shows the current stage |
| Sensor calibration | Menu > Pressure sensor | The two-point calibration. Row 1: Signal (mV/V), Pressure (mbar, with the calibration in use) and Saved cal (the pressure by the saved calibration, In use when that is the one in use, None before the first save). Rows 2 and 3: the vacuum and air points, each with its Capture button; a point captured since the last save has a dark edge on the left, as a draft setting |
| Diagnostics | Menu > Diagnostics | Live raw readings, no row captions, Help at the top right (0.16.27). Row 1, the transducer in signal order: Excitation (V), Bridge (the Druck output, mV), Raw (the pressure before calibration, mbar; near Room in air). Row 2, the valve motor: Motor steps, Limit switch (Pressed or Released) and Last move (how it ended and how long it took, for example At target, 41.3 s). Row 3: Room (the Pressure 21 room sensor, mbar) and Board temperature. ---- means no reading |
| PID tuning | Home > PID tile, or Menu > PID tuning | Gains tab (PID to 0.16.24): four set cards, Set 1 to Set 4 each over its pressures (the one shown outlined, a dot before the name of the set in use), over that set's KP, TI and KD (captioned KP 1, TI 1, KD 1 and so on). Set ranges tab (PID table to 0.16.24): Set 1 \| 2, Set 2 \| 3 and Set 3 \| 4 (boundaries 1 to 3, mbar; None for a boundary 3 of 0) in a row, Hysteresis under them with a line saying how the set is picked; the table is always on (0.16.20). AUTO tab (AUTO limits to 0.16.24): AUTO min and Pressure response, SP band (the setpoint band; None for 0) under them with a line saying what it is (section 10) |
| Motor settings | Menu > Motor settings | The valve's saved numbers on three tabs: Drive (run and start speeds, acceleration; the first tab from 0.16.21), Travel (travel, direction, deadbands) and Homing (back-off, return, close speeds); these move nothing, and Help says so and points at Valve position (section 10). A fourth tab, Override: first Disable switch From to To % (0.16.22; Ignore switch in 0.16.21, Switch pass from 0.16.19), a toggle for a valve that binds part way and trips the limit switch off its seat. While it is On and the valve is homed, the limit switch does not stop closing from From to To % open and raises no W-1, W-4 or F-4; outside that it works as always. Its line reads Off: switch stops closing; On: still stops below From (On: stops by count at 0 % with From 0); or On: works once homed. A press on the card turns it on or off at once; it is off after every restart. Under it the From and To cards (% open, 0 to 100, 5 and 50 by default; saved settings from 0.16.21, fixed before) and the Motor override button (dark outline): it asks first, then opens the Motor override page, which drives the motor by steps; Back returns to the Override tab |
| Review and save | Menu > Review and save | Last and previous save, the unsaved changes, Save settings, Discard changes and Load settings (last save, previous save or factory defaults) (section 10) |
| System | Menu > System | Cryostat name and Remote control on the first row, IP address and MAC address on the second, Firmware with Restart and Shut down beside it (section 10) |

Display formats: pressure to 1 decimal in mbar, valve position and keypad entries to 2 decimals in %, dates DD/MM/YY.

Keypads: a tap on a value box opens a number pad as a popup over the dimmed page, with the title and the present value on the left and the entry in a blue-edged field on the right. Set applies the value. A value outside the range is refused with "Out of range" and the range in place of those lines. Cancel, or a tap on the dimmed page beside the pad, leaves the old value.

Short messages: a refused action puts its reason in the strip for a few seconds, for example "Stop AUTO first", "Home the valve first", "Reset the fault first", "Limit switch pressed, cannot close" or "Wait for the valve to stop".

## 6. MANUAL and AUTO, setting the target

### MANUAL

In MANUAL the operator positions the valve and watches the pressure. Position is in percent of full travel: 0 % is just above the seat, 100 % is 4 output turns open by default.

- Tap the Go to box on Home to type a position, then Set. 0 and 100 close and open the valve fully.
- Moves run at the drive's run speed (Menu > Motor settings > Drive). A new Go to during a move retargets it: further on extends the move, behind it ramps down and turns round.
- The limit switch stops any closing move. If it trips away from 0 % the controller asks Set as seat or Skip (W-1, section 9).
- Go to needs the valve homed or its position set. Before that a tap says why and nothing moves. Motor override (Menu > Motor settings > Override) can move the motor by steps without a position reference.

### Starting AUTO

AUTO needs the valve homed (or its position set), no fault latched and a valid pressure reading. Press Start AUTO in the Pressure tile on Home.

- AUTO starts at once at the pressure shown, as the target, with no question. The valve starts from where it is.
- For another target, tap the SP box that then takes Start AUTO's place and type it; Set applies it at once.
- Start AUTO is grey until the valve is homed, with a valid reading and no fault. A refused start says why in the strip: Home the valve first, Reset the fault first, No valid pressure, Set a target first, Target outside the sensor range, Pressure above the sensor range, Pressure response not set, Tuning not set, Limit switch pressed away from the seat, Limit switch question waiting at the screen, Valve being moved on the screen (Close or Open held), or Sensor calibration point being taken.

The entry is bumpless: the loop starts from where the valve is, so the first move is small.

### In AUTO

Once a second, after the pressure reading, the controller works out a new valve position and moves to it. The strip reads Regulating to X mB, then In band, X mB when the pressure is within the setpoint band (0.1 mbar by default; with a band of 0 it stays Regulating), then Stable at X mB after 60 s in the band. The Trend page shows the target as a dashed line.

- Change the target from the SP (setpoint) box on Home. Set applies it at once, with no question, however large the change.
- The valve moves in short bursts rather than continuously. Small reversals are held until the command is 0.2 mbar of error at the set's KP from the valve, and never more than 0.1 % of travel, so the reading's noise does not turn the motor round inside the drive's backlash (firmware 0.12.12; scaled with KP since 0.16.9, so at the low KPs of the low pressures it is much smaller: 0.012 % at KP 0.06). The motion is still less smooth than the Mercury iTC's; this is a known difference (26 October 2026).
- In AUTO the Valve tile has Stop AUTO in place of the Go to box. Homing and changes to motor settings are refused in AUTO with "Stop AUTO first". PID tuning drafts (and the two deadbands on Motor settings) can be saved and take effect from the next control step; other changed valve drafts must wait for MANUAL.
- With the PID table on (Menu > PID tuning > PID table), the loop uses PID set 1, 2 or 3 by the pressure. The PID tile on Home shows the KP, TI and KD in use (no set number); a change of set does not move the valve and is logged in Events.
- The controller watches for a pressure that does not respond to valve moves (F-6), a pressure that does not settle in 30 min (W-2), and a pressure above the sensor range (W-5). Each of these ends AUTO; section 9 says what to do.

### Leaving AUTO

- Stop AUTO in the Valve tile on Home goes to MANUAL at once, with no question. The valve ramps down if it was moving and holds where it stops. The target is kept but not used: the next Start AUTO takes the pressure shown.
- STOP does the same.
- A fault, W-2, W-4 or W-5 also ends AUTO. The valve stops and the strip says why.
- Loss of the motor 12 V supply is not detected directly. AUTO and the step count can continue while the motor is stationary. F-6 may later detect a lack of pressure response, but it is not a motor-supply detector.

When AUTO ends for any reason the valve stays where it was. It does not close. If the cryostat needs the valve elsewhere, move it in MANUAL.

## 7. Homing the valve

Homing finds the closed limit switch and sets 0 % from it. It is the only way the controller checks the valve position. Home after every power-up, after a motor direction or coil order change, and whenever the position is in doubt.

Start it from Home valve in the Valve tile on Home (while NOT HOMED) or from Menu > Valve position. The question "Home the valve? It closes onto the limit switch to find 0 %, up to N min" confirms it. The Valve position page shows the stage running and output turns. Diagnostics shows the motor step count and live limit switch.

1. If the switch is already pressed, the valve moves off it first. If the switch stays pressed, fault F-4.
2. Fast close to the switch. If the switch is not found within 1.1 times the full travel, fault F-3.
3. Back off the switch by the back-off distance (400 full steps by default).
4. Slow close onto the switch. This point is the seat.
5. Open by the separate Return distance (400 full steps by default), then check that the switch has released. This last point is 0 %. Return and Back-off are independent settings under Menu > Motor settings > Homing. The seat reads `-100 × Return / full-travel steps` %, about -0.6 % with the defaults. A switch still pressed after the initial clearance, Back-off or Return raises F-4.

After any initial clearance, the strip reads "Homing 1 of 4: fast close", "Homing 2 of 4: backing off", "Homing 3 of 4: slow close" and "Homing 4 of 4: return" as it goes. STOP cancels homing and keeps the earlier homing, if there was one.

### Setting the position without homing

The Valve position page also offers three ways to set the position without moving the valve. None of them is checked against the limit switch, and the Valve position page shows "Set by hand, not homed" or "Last saved, not homed" afterwards.

| Option | When to use it |
| --- | --- |
| Set as 0 % | When the valve is where 0 % should be. The present position becomes 0 % and reads 0.00 % |
| Set as other % | When the position is known, for example from the previous run |
| Use last saved | The position saved before power was lost. Offered after start-up until the valve moves or is homed. If power was lost during a move the prompt says how far out it may be (at most about 2 s of travel) |

Home the valve when you can. Only homing checks the position. After a motor direction or coil-order change, entering or restoring a position is refused until the valve has been homed.

### Limit switch during a move

If the switch trips during a normal closing move away from 0 %, the valve stops and the strip asks "Limit switch at X %. Set as seat?" with Skip and Set as seat (W-1). Set as seat takes the switch point as the seat, which reads about -0.6 %, as after homing. The count is never zeroed on its own.

- Set as seat when the valve really is at the seat, for example after a direction change or a slipped drive.
- Skip when the switch tripped early (ice, a stiff drive). The count is kept. Open the valve, let the ice clear, and home again later.

A switch that reads pressed with the valve well off the seat raises W-4 (section 9).

## 8. Sensor calibration

The Druck output is ratiometric, so the controller reads the ratio of signal to excitation in mV/V and turns it into pressure with a two-point calibration: a vacuum point and an air point. Both points are read live through the board as wired, so the board's own loading, gain and offset sit inside the calibration. With no calibration saved the nominal 0 to 100 mV at 10 V is used. Menu > Pressure setup shows the nominal conversion and saved state; readings were about 5 % low on the bench with this conversion.

Calibrate when a sensor is first fitted to a board, when the sensor or the Click boards are changed, and when a check against the reference gauge shows drift. Calibration is refused in AUTO.

From the screen (Menu > Pressure sensor, the Sensor calibration page):

1. Pump the sensor down. Press Capture vacuum. The question "Sensor pumped down? Capture records 0 mB" has Capture, which takes the point as 0 mbar, and Enter value (Not 0 mB to 0.16.26), which opens a keypad for the reference gauge reading (0 to 50 mbar). Type what the Pfeiffer gauge shows. The capture averages 5 readings and takes about 5 s.
2. Vent the sensor to the room. Press Capture air. With the Pressure 21 room sensor fitted, the question "Sensor open to room air?" shows its reading: Capture takes the air point at the room sensor's reading averaged over the same 5 readings, and Enter value opens a keypad for a laboratory barometer reading instead. Without the room sensor the keypad opens straight away: type the barometer reading in mbar, for example 1013.2 (500 to 1100 mbar).
3. Once both points are held the new calibration is in use and unsaved. Compare Pressure (the new calibration) against Saved cal on the page.
4. Press Save settings (Menu > Review and save). Discard changes puts the last save back.

Either point can be redone on its own; the other is kept.

The calibration is taken on the screen only; the text link cannot change it.

The board holds one sensor, the 15 psia Druck, with one calibration (0.16.2; to 0.16.1 up to four sensors could be kept and chosen under Settings > Choose sensor). A replacement sensor is calibrated in place: capture the two points again and save. A record saved with several sensors loads as the one that was in use, and its serial number is kept for the calibration name.

The Pressure 21 Click in S1 gives the controller its own room-pressure reading (BAROMETER?). It is used for the air point. The last recorded bench notes did not confirm fitting or operation of the Click (section 13).

## 9. Faults and warnings

Faults latch until Reset fault (on the strip, on Events, or in the fault detail), which clears only those whose cause has gone. The strip shows the highest-priority fault; the number order is the priority order. Any fault in AUTO stops the valve and switches to MANUAL. F-1 and F-2 are sensor faults and do not block valve moves, so the valve can still be moved and homed with no pressure reading. F-3 to F-6 block moves until reset. Tap a fault on Events for its detail: what happened, what the controller did, what to check, and when it can be reset.

| Fault | Cause | What the controller did | Check | Reset |
| --- | --- | --- | --- | --- |
| F-1 No pressure signal | The Druck reading is missing or outside 0.005 to 11 mV/V | Pressure shows dashes. The valve still moves. AUTO ends | The Druck cable and plug, and the Power Monitor Click | When the reading is back |
| F-2 Excitation | The Druck excitation left 9.5 to 10.5 V, or the Boost 10 tripped | Pressure not trusted. The valve still moves. AUTO ends | The Boost 10 and the board's 5 V. A trip needs a power cycle | When the excitation is back in the window |
| F-3 Limit not found | Homing closed 1.1 times the full travel without the limit switch | Stopped. The valve is not homed | The switch and its wire, and that the valve turns | Any time. Then home again |
| F-4 Limit pressed when it should be free | The limit switch stayed pressed after the valve moved off it, or was pressed again during an opening move | Stopped. The valve is not homed | The switch is not stuck (ice) and its wire is not broken. After an opening move: the motor direction and coil order (Motor override), as the valve was in fact closing | Any time. Then home again |
| F-5 Motion timeout | A move took longer than its distance at the set rate plus the ramps plus 2 s | Stopped the valve and holds it. Homing kept, unless it came while homing | The valve turns freely, and the motor 12 V supply | Any time |
| F-6 No response | The valve moved in AUTO but the pressure did not respond over 5 windows of 30 s | Stopped the valve and holds it. MANUAL | Drive slipping, motor unplugged, needle seized, helium supply lost | Any time. Check before starting AUTO again |

Warnings do not latch. W-2, W-4 and W-5 show on the strip until OK is pressed or the next Start AUTO, and each ends AUTO. W-1 clears when its question is answered. W-3 clears on its own.

| Warning | Cause | What the controller did | What to do |
| --- | --- | --- | --- |
| W-1 Limit switch pressed at X % | The switch tripped during a closing move away from 0 % | Stopped the valve and asks Set as seat or Skip, over any screen | Set as seat if the valve is at the seat. Skip if the switch tripped early (ice). Section 7 |
| W-2 Not settling | 1800 s outside the setpoint band (0.10 mbar at least) in AUTO | AUTO ended, MANUAL, valve idle where it stopped | Check the pump, the helium supply and the target against what the cryostat can reach. Press OK. Start AUTO again when ready |
| W-3 Controller hot | Power Monitor die at or above 55 °C | Nothing blocked. Shown until it cools below 50 °C | Check the enclosure ventilation |
| W-4 Switch pressed off the seat | The switch reads pressed with the valve well off the seat for 0.2 s | In AUTO: AUTO ended, valve idle. In MANUAL: a strip warning | Check the switch, its wiring and the valve position. Home again when the switch is free |
| W-5 Over range in AUTO | The pressure is above the sensor range, two reads in a row | AUTO ended, MANUAL, valve idle | The line is at or near atmosphere. Pump down, then start AUTO |

Other strip messages:

- **Limit override held / closing past the switch**: the override from Motor settings > Motor override is in use. Release either finger to stop a closing override; there is no Cancel button on the strip.
- **Valve not homed**: the valve position is not known.
- **Shut down stopped: limit switch pressed before 0 %**: the limit switch tripped while Shut down moved the valve to 0 % (section 4). The limit switch question follows. OK clears it.

The event log (Menu > Events) keeps the last 100 events with a time stamp: faults, warnings, mode changes, moves, settings saves, calibration captures and remote commands with the client's IP address. It is in RAM and empties at a restart.

## 10. Settings

Settings are commissioning items, not daily operation. Numeric valve and tuning edits are drafts until Save settings, which checks them together and saves all or nothing. The name, IP address, Remote control switch and captured calibration take effect before saving and remain unsaved until Save settings; a changed IP address is used after a restart. The text link does not change settings. In AUTO, Save settings accepts PID tuning drafts; changed valve drafts answer "Stop AUTO first". Live changes such as the name and Remote control switch are also included in the saved record. The board keeps the last two saves, in two flash sectors in turn, and reads a save back before it counts.

| Page | Settings | Notes |
| --- | --- | --- |
| System (on the Menu) | Cryostat name (up to 12 characters, shown in the Home title). IP address and mask. MAC address. Remote control switch (On by default). Firmware version and build date, with Restart and Shut down | The name is in use at once and unsaved until Save settings. A new IP address is used after Save settings and a restart. Default puts the build's address back |
| Motor settings, Travel tab | 100 % travel in output turns (4 by default, 16512 steps per turn). Motor direction. Deadband open and Deadband close (full steps, 8 each by default) | Changing the direction clears the homing. Home again after saving. AUTO holds an opening move until it adds up to Deadband open, then makes it in one go, so a needle that sticks when opening (seen at low openings on the 5 November bench, 0.3 to 0.44 % to break free) is moved past its stick at once rather than crept up to; closing moves smaller than Deadband close are skipped. The deadbands can be saved in AUTO |
| Motor settings, Homing tab | Back-off (400 full steps), Return (400 full steps), fast close speed (400 steps/s), slow close speed (25 steps/s) | Back-off follows the fast close; Return follows the slow close and defines 0 %. The distances are independent |
| Motor settings, Override tab | Disable switch From and To (% open, 5 and 50 by default; From 0 to 99, To 1 to 100 from 0.16.22, From below To) | Saved with Save settings (0.16.21; 5 to 50 % fixed before; settings saved before 0.16.21 load 5 and 50). With From 0 closing stops only at 0 % by the step count. The toggle itself is not saved: off after every restart |
| Motor settings, Drive tab | Run speed (400 steps/s), start speed (200 steps/s), acceleration (3200 steps/s²), by default | Used by MANUAL and AUTO moves. Saved when the valve is stopped |
| PID tuning, Gains tab | KP, gain (% open per mbar). TI, integral time (min; 0 is off). KD, derivative (% open per mbar/s of pressure change; 0 is off). Factory (0.16.18): set 1 KP 0.06 TI 0.60, set 2 0.12/0.82, set 3 0.80/0.94, set 4 0.40/0.25, KD 0 in every set (KP 4.00 TI 0.25 in every set to 0.16.17). The same for each of sets 1 to 4, picked on the set row (each set shows its pressures, a dot on the set in use) | Set at commissioning. Operators do not change these. KP acts on the pressure, not the error (0.16.15): a setpoint change does not move the valve at once; the integral takes it to the new setpoint, so the pressure comes in from one side without the bounce of the kick being given back. KD acts on the pressure's rate of change, so a setpoint change gives no kick from it either. Settings saved before 0.14.21 load with KD 0; settings saved before 0.15.15 load with all sets equal to the saved gains, so AUTO runs as it did; settings saved from 0.15.15 to 0.16.13 load with set 4 equal to set 3 and boundary 3 at 0 (three sets, as they ran) |
| PID tuning, Set ranges tab | The PID table is always on (0.16.20): the set is picked by the lower of the pressure and the setpoint. To 0.16.19 it could be set Off (set 1 always); settings saved with it Off load with it on, and the factory setting was On from 0.16.18. Hysteresis (% of the boundary, 15), then B1, B2 and B3, boundaries 1 to 3 (mbar, 6.0, 10.0 and 14.0): set 1 below B1, set 2 from B1 to B2, set 3 from B2 to B3, set 4 above B3. B3 at 0 gives three sets (set 3 above B2, set 4 unused) | Gain scheduling on the pressure: the loop gain of the cryostat changes with the pressure, so one set of gains cannot suit 1 and 25 mbar. Four sets from 0.16.14 (bench 11 Nov: the 8 to 16 mbar region wants a set of its own between the low and the high pressure sets). A set changes only once the pressure is past the boundary by the hysteresis (with 6, 10 and 14 mbar and 15 %: up at 6.9, 11.5 and 16.1 mbar, down at 11.9, 8.5 and 5.1 mbar), so noise at a boundary cannot flip it. The set follows the lower of the pressure and the setpoint (0.16.9): going up the pressure decides, as before; going down the setpoint's set takes over as soon as the setpoint is changed, so a big step down does not start on the high-pressure gains and shut the valve. A change of set does not move the valve by itself (the integral is re-based), and neither does a setpoint change, in a set or across sets (0.16.15; 0.16.13 and 0.16.14 moved the valve by the new set's KP times the change). The change is logged, for example "AUTO PID set 3 to 2, pressure 16.9 mbar". Boundary 2 must be above boundary 1, and boundary 3 above boundary 2 (or 0), by more than the hysteresis either side. The setpoint table (0.15.15 to 0.15.19) is removed; settings saved with it load with the PID table on the pressure. For one set of gains at every pressure, give all four sets the same gains |
| PID tuning, AUTO tab | AUTO min (%), pressure response (opening raises or lowers the pressure), Setpoint band (mbar, 0.10 by default, 0 to 10; 0 = none: In band and Stable never show) | Set at commissioning. A wrong pressure response gives W-2 after 30 min in AUTO; W-2 judges against 0.10 mbar at least, so a band under 0.10 does not end AUTO. The deadband moved to Valve settings (then Valve setup, now Motor settings) in 0.15.15 |
| Review and save | Last save and previous save by date. Unsaved changes list. Save settings. Discard changes. Load settings: last save, previous save or factory defaults | A loaded save is in use at once, sensors and calibrations included. Factory defaults come in as drafts and keep the sensors. Load is refused while the valve moves or in AUTO |
| Restart (System, and the power icon on Home) | Stops the valve, ends AUTO or homing, then restarts after about 2.5 s | Asks first. Saved settings are loaded and the valve starts NOT HOMED, or OFF after a restart while OFF |
| Motor override (Motor settings > Override) | Move tab: step size 1 to 10 000 or Hold, Close and Open, Zero, Limit override. Speed tab: run, start and acceleration rates. Coils tab: step mode (wave, full, half), hold torque, coils off, coil order, direction. (The Switch tab, 0.16.5 to 0.16.18, is now the Disable switch toggle on Motor settings > Override (Switch pass to 0.16.20, Ignore switch in 0.16.21).) | Engineering page. It drives the motor by steps past the valve's checks. Coil order, direction and Zero clear the homing and ask first. The strip reads the motor state and the limit switch |

Factory defaults for the valve and the loop are for the PK22 344:1 stepper on the Oxford Instruments needle valve. The CF-11 on the bench ran at about 1.6 K with 3 to 8 mbar, the valve around 34 % open, on the Mercury iTC's P 20 % and I 0.1 min. The mm4's equivalent gain and integral time for each cryostat are set at commissioning.


## 11. Remote access

The Ethernet build (the one for the cryostat) runs two servers on one cable and one IP address. The USB build has the text link on the USB-C port instead and no Modbus. The two cannot run together.

### Network and IP address

The controller has a fixed (static) IP address. The address is the one saved under Menu > System, or the build's default 192.168.1.100 with mask 255.255.255.0. There is no gateway, so it answers only on its own subnet. Home shows the address in use as address/prefix (192.168.1.100/24); Menu > System shows it with the MAC address. There is no DHCP or DNS, so connect by the IP address.

### Remote light

Two small arrows in the header, left of the mode panel, show remote traffic: the arrow down is white while requests arrive (text port lines or Modbus requests), the arrow up while replies are sent. Each stays lit until 3 s after its last message, so a client polling every few seconds gives a steady light, not a flash per message; grey means nothing in the last 3 s. The light is shown while a client is connected to the text port or to Modbus, and for 10 s after the last message (the USB build has no connection to see, so it shows while lines arrive). A set command that is taken sends no reply, so set commands alone light only the arrow down. A tap on the light opens System, with the Remote control switch and the address. With Remote control Off the arrows still light: the requests arrive and are answered with ERR 211.

To set the address, type it and the mask on the System page (IP address), then Save settings and restart. The address cannot be changed over the network, so a remote user cannot cut off their own link. Put the PC on the same subnet with a fixed address: for the default, for example 192.168.1.10 with mask 255.255.255.0. On a site network, give each controller its own address on the instrument subnet: they all start at 192.168.1.100.

Ethernet and Modbus were proven on the bench on 26 October 2026 with the laptop on a fixed address.

### Port 8888: text commands

Port 8888 takes the remote command set in plain text, one command per line (upper case, ending in LF or CR LF, at most 96 characters), for a supervisory program or a person at a terminal (PuTTY in Raw mode). One client at a time; a new connection replaces the old. There is no echo and nothing is sent unasked: faults, warnings and switch changes go to the event log. A query (ending in ?) answers one line that starts with its own words. A set command answers nothing when it is accepted, and ERR with a code and a reason when it is refused. Several commands can share a line, separated by ;, and their query replies come back on one line.

| Group | Commands |
| --- | --- |
| Reading | PRESSURE? the pressure. SETPOINT? the setpoint. MODE? MANUAL, AUTO, HOMING, NOT HOMED, SHUTDOWN or OFF. VALVE? the position; VALVE MOVING? 1 while moving or homing. ALARM? the latched faults and the warnings. EVENT? followed by a number, the last events (on a line of its own). BAROMETER? the room pressure. NETWORK? the address in use. TIME? the clock. PID? the PID gains in use (KP, TI, KD of the set in use). PIDS? the set in use, then KP, TI, KD of sets 1 to 4 (13 numbers). SCHED? the PID table (PV, on the pressure; always from 0.16.20), boundaries 1, 2 and 3 in mbar, hysteresis in %. *IDN? the identity and firmware version |
| Control | SETPOINT followed by the setpoint in mbar (does not start AUTO). MODE AUTO starts AUTO with the same checks as the screen; MODE MANUAL ends it, the valve stays where it is. VALVE SET followed by a % moves the valve in MANUAL. STOP stops the valve and ends AUTO or a shut down; it is the only stop. ALARM RESET clears the faults whose cause has gone; ALARM OK acknowledges a strip warning. TIME SET 2026-10-27 09:00:00 sets the clock. PID SET 3.5,0.5 sets and saves KP and TI (a third number sets KD) of the set in use, as Save on the PID tuning page; it is taken in AUTO at the next control step. PIDS SET 2,0.8,0.3 does the same for set 2 (or 1, 3 or 4). SCHED SET PV,6,10,14,15 sets the PID table: PV (OFF answers ERR 102 from 0.16.20), boundaries 1, 2 and 3 in mbar (boundary 3 0 for three sets), hysteresis in %; the four-number form of 0.15.15 to 0.16.13 answers ERR 101. Each change is a save in flash: send it only when the values change |

Set commands need Remote control On (Menu > System); with it Off they answer ERR 211, STOP included. Homing, calibration, the other settings and the motor are on the touchscreen only. In AUTO, VALVE SET answers "Switch to MANUAL first". While OFF or shutting down, VALVE SET, MODE AUTO and MODE MANUAL answer ERR 213; the controller is turned on at the screen only. The error codes and the full table are in the README. With network writes enabled, any other command answers ERR 100. With writes off, queries still work but other input, including an unknown command, answers ERR 211. The USB build is local and is not gated by the Remote control switch.

Tools in the repository: tools/logger/log_druck.py polls the pressure, target, valve, mode and alarms once a second and logs them to CSV, from an IP address or a COM port, and reconnects on its own.

### Port 502: Modbus TCP

The Modbus server is for a supervisory system. It reads 59 holding registers (0 to 58, map version 3 from 0.16.14; version 2, 0.15.15 to 0.16.13, ended at 50) and 14 coils, and writes only the target, the valve position in MANUAL, the PID sets and PID table (registers 27 to 58: set 4's gains at 51 to 56, boundary 3 at 57), and the AUTO, STOP, RESET and warning OK coils. Homing, the switch question, calibration and the other settings are not on Modbus, because each needs a person watching or changes what the controller trusts.

- Function codes 1, 3, 5 and 16. Up to 2 clients at once. Unit id not checked. Addresses are 0-based.
- Floats are IEEE 754 single precision in two registers, high word first. NaN means no value.
- Values update once a second, so polling faster than 1 Hz adds nothing. Read registers 0 to 58 in one request.
- Register 1 is a heartbeat (seconds since start-up). A value that stops changing means the controller has stopped.

| Address | Content |
| --- | --- |
| Register 2 | Mode: 0 NOT HOMED, 1 HOMING, 2 MANUAL, 3 AUTO, 4 SHUTTING DOWN, 5 OFF |
| Register 3 | Status bits: reading valid, position set, homed, moving, switch pressed, fault, warning, target set, writes on, legacy motor-supply bit (always 1; supply is not measured) |
| Register 4 | Regulation in AUTO: 1 Regulating, 2 In band, 3 Stable |
| Registers 5, 6 | Faults latched (bit 0 F-1 to bit 5 F-6) and warnings showing (bit 0 W-1 to bit 4 W-5; bit 5, W-6, is always 0 since 0.15.11) |
| Registers 10 to 11 | Pressure, mbar |
| Registers 12 to 13 | Target, mbar (writable, any mode) |
| Registers 14 to 15 | Valve position, % |
| Registers 16 to 17 | Valve going to, % (writable, MANUAL only) |
| Registers 20 to 23 | Excitation V and controller temperature degC |
| Register 26 | PID set in use, 1 to 3 |
| Registers 27 to 32 | PID table: 2, on the pressure (0, Off, refused from 0.16.20; 1, the setpoint, from 0.15.20), boundary 1 and boundary 2 (mbar, floats), hysteresis % (writable, any mode, saved) |
| Registers 33 to 50 | KP, TI, KD of sets 1, 2 and 3, floats (writable, any mode, saved) |
| Coils 0 to 3 | AUTO (1 starts, 0 switches to MANUAL), STOP, RESET, warning OK |

A write the controller refuses answers exception 4 and logs the reason as a NET event, with the client's IP address. Exception 1 means the function code is not supported or writes are off; 2 an address outside the map or a read-only register; 3 a bad value or count. The complete map with every bit is docs/modbus-register-map.md.

The Modbus read test passed on the bench on 26 October 2026. The write test has not been run (not benched). What the controller should do if the supervisory system stops polling is still to be agreed with its owner; today it does nothing.

## 12. Maintenance

| Task | When | How |
| --- | --- | --- |
| Check the pressure reading against the reference gauge | At each cool-down, and whenever the reading looks wrong | Compare the controller's reading with the Pfeiffer gauge at vacuum and at air. Recalibrate (section 8) if either point is off by more than the sensor's accuracy |
| Home the valve | After every power-up that does not keep the position from Shut down, after a direction or coil order change, and when the position is in doubt | Section 7 |
| Check the limit switch | When F-3, F-4 or W-4 appears, and at each warm-up | The Valve position page or Diagnostics shows pressed or free. Press the switch by hand and watch it change. Look for ice on the switch and the valve drive |
| Check the Druck excitation | When F-2 appears | Diagnostics shows the excitation. It should be 9.5 to 10.5 V. A Boost 10 trip needs a power cycle |
| Check the motor supply | When moves are slow, F-5 appears or the motor is noisy | Measure the 12 V at the Stepper 3 Click. The controller does not measure it |
| Review the event log | After a fault, and before clearing it | Menu > Events, or EVENT? 100 over the link. The log empties at a restart, so read it first |
| Save the settings | After any commissioning change | Menu > Review and save > Save settings. Review and save shows the date of the last save |
| Set the clock | When the dates on saves and events are wrong | TIME SET over the link, in the form TIME SET 2026-10-27 09:00:00 |
| Update the firmware | When a new version is released | Flash over USB-C with mikroBootloader. The settings, calibration and saved position are in serial flash and survive reflashing. Check Menu > System, or *IDN? over the link, afterwards |
| Clean the touchscreen | As needed | Shut down and switch off first, or the taps will act on the screen (a double tap on the OFF screen turns it on). A soft cloth, no solvents |

The controller has no fuse, battery or filter to replace. The real-time clock runs from the board supply; a cell in the BAT1 holder keeps it through a power-off.

If the controller stops responding, the independent watchdog restarts it within about 4 s. The current Diagnostics page does not display the reset cause; it shows raw readings only. Every restart comes back OFF, so the controller stays off until someone double taps the screen; AUTO does not resume by itself. The valve does not move after the restart until it is homed or its position is set, and a position saved before the reset is offered.

## 13. Items not yet benched

The firmware is a prototype. The last recorded bench status below is from 27 October 2026; no new bench evidence was added by this documentation revision. Source-described behaviour in this manual is for firmware 0.14.8, but that is not confirmation of hardware operation.

| Item | State |
| --- | --- |
| Pressure 21 Click (room pressure, BAROMETER?) and the air calibration reference from it | Driver built in 0.11.47. Click ordered 15 October; fitting and operation were unconfirmed in the 27 October bench notes |
| Modbus writes (target, valve going to, coils 0 to 3) | On since 0.12.6. The read test passed 26 October; the write test has not been run |
| AUTO on a cryostat with the production gain and integral time | AUTO has run on the bench rig against a manual vent valve, and its motion was compared with the Mercury iTC on 26 October. Tuning for each cryostat is still to be set |
| W-5 Over range | Checked on the PC control-loop test, not provoked on the bench |
| Loss of polling from the supervisory system | No behaviour defined yet. Waiting on the owner of that system |
| Earth bond in the enclosure | Found on the bench 21 October; not yet in the enclosure drawings |
| Shut down, OFF and the power-up after it (0.15.0, 0.15.1) | Checked in the simulator only. To check on the bench: the move to 0 % and the coils off, the power-up after it with the switch released and pressed, the black screen from the first frame, the double tap on the real touch panel |

## 14. Revision history

| Revision | Date | Change |
| --- | --- | --- |
| A | 27 October 2026 | First issue, from firmware 0.12.17 and the design documents listed in the reference block |
| B | 2 November 2026 | Firmware 0.14.8: the text link takes the remote command set only (engineering commands, the step test and the typed calibration table removed) |
| C | 3 November 2026 | Checked against firmware 0.14.8 on main: corrected movement prerequisites, independent homing Return and four stages, motor-supply behaviour and legacy status bit, tuning saves in AUTO, motor defaults, current screens and labels, remote refusals and version references. Historical bench status retained separately |
| D | 3 November 2026 | Firmware 0.14.9 to 0.14.11: Ethernet on a static IP address only, default 192.168.1.100/24. Firmware 0.14.12: the Faults and log screen is renamed Events. Firmware 0.14.13: the fault detail's Next fault, Reset fault and Close buttons show (they were hidden under the status strip). Firmware 0.14.14: Set as 0 % reads 0.00 %, not the seat (about -0.6 %). Firmware 0.14.15: the Valve position state reads Set by hand or Last saved, not homed. Firmware 0.14.16: a STOP on the screen no longer holds remote moves (no Allow on the strip). Firmware 0.14.17: the limit switch question's button reads Set as seat. Firmware 0.14.18: the Valve screen's buttons use the same column widths as the other screens. Firmware 0.14.19: a tap during a slow redraw (Trend) no longer leaves the button lit without acting. Firmware 0.14.20: on Home, the Setpoint label reads SP, and SP and Go to are at the button text size |
| E | 4 November 2026 | Firmware 0.14.21: on Home, the Valve button is replaced by the PID tile (KP, TI and KD in use, a tap opens PID tuning) and Menu moves up a row; Maintain replaces the grey SP box in MANUAL and starts AUTO at the pressure shown; Control tuning is renamed PID tuning, with PID and AUTO limits tabs; KD is a saved setting (0 by default); the Valve page is removed, Go to on Home stays. Firmware 0.14.22: a new setpoint in AUTO applies at once; the "Change setpoint to X mB?" question for changes over 5 mbar is removed. Firmware 0.14.23: the header shows the mode as MANUAL mode or AUTO mode in plain lettering, with no box, so it is not taken for a button |
| F | 4 November 2026 | Firmware 0.15.0: Shut down (the power icon on Home) closes the valve onto the limit switch, takes it as the seat, switches the coils off and turns the controller OFF (black screen, double tap to turn on). A power-up after Shut down with the switch still pressed starts in MANUAL at the seat. New modes SHUTTING DOWN and OFF (MODE? SHUTDOWN and OFF, Modbus mode 4 and 5) and ERR 213. The start-up question says when the controller was switched off without Shut down |
| G | 4 November 2026 | Firmware 0.15.1: Shut down moves the valve to 0 % instead of closing onto the limit switch (the 1 % seat check is gone); not homed, it turns OFF without moving the valve. Every power-up and restart starts OFF, black from the first frame; the start-up logo and bar show when it is turned on. After Shut down the position is kept at power-up if the limit switch reads released. The power icon moves to the top left of Home |
| H | 4 November 2026 | Firmware 0.15.2: the header shows the mode word again (MANUAL, AUTO, NOT HOMED, HOMING, FAULT) on a flat panel the full height of the header, as wide as Start AUTO below it: grey, white in AUTO, red with a fault. It replaces the plain MANUAL mode / AUTO mode lettering of 0.14.23 and does not look like a button |
| I | 4 November 2026 | Firmware 0.15.3: Home has no mode button. Start AUTO (was Maintain) is in the Pressure tile and starts AUTO at the pressure shown; Stop AUTO is in the Valve tile in AUTO and goes to MANUAL; Home valve is in the Valve tile while not homed. The right column reads PID tile, Trend, Menu, STOP. Screen messages say "Stop AUTO first" (the network keeps ERR 202 "Switch to MANUAL first") |
| J | 4 November 2026 | Firmware 0.15.4: the header's AUTO panel is dark blue with white lettering (white with dark lettering in 0.15.2 and 0.15.3). Home redraws at once after Start AUTO or Stop AUTO instead of at the next half-second refresh |
| K | 4 November 2026 | Firmware 0.15.5: the header's mode panel reaches the screen's right edge and starts the same 10 px left of the button column (x 351 to 480), the word centred over the column |
| L | 4 November 2026 | Firmware 0.15.6: on Home the IP tile (the IP address and subnet mask in use, a tap opens System setup) replaces the Trend button. Trend stays on Menu and on a tap on the sparkline |
| M | 4 November 2026 | Firmware 0.15.7: the Shut down question has a "Close the valve to 0 %" checkbox, ticked each time; unticked, the controller turns OFF with the valve left where it is and its position kept. At power-up the backlight stays off until the OFF screen is drawn (the panel flashed white before) |
| N | 4 November 2026 | Firmware 0.15.8: the text port takes PID? (the KP, TI and KD in use) and PID SET (set and save them, as Save on the PID tuning page, in AUTO too), for a gain schedule run from a PC |
| O | 5 November 2026 | Firmware 0.15.9: the common screen template. Header: Home at the top left on every page (the power icon on Home), Back beside it, the title left-aligned. The power question also offers Restart. Menu rows Records (Trend, Events), Service (Valve position, Diagnostics), Setup (Settings, System). Settings holds navigation tiles only; System moves to the Menu with Restart; Motor override moves to Valve position. Keypads are a popup with the present value and the entry side by side. The Home IP tile reads "IP address" over address/prefix, for example 192.168.1.100/24 |
| P | 5 November 2026 | Firmware 0.15.10: the Home IP tile is left-aligned, as Menu |
| Q | 5 November 2026 | Firmware 0.15.11: W-6 (driven the wrong way) is removed. It ended AUTO at low pressure, where the valve rightly rests near closed with the pressure under the band. A pressure response setting the wrong way round now shows as W-2 after 30 min |
| R | 5 November 2026 | Firmware 0.15.12: the Home IP tile is centred again |
| S | 5 November 2026 | Firmware 0.15.13: the header's mode panel starts at the right edge of the third column (the Valve card's right edge on Home) |
| T | 5 November 2026 | Firmware 0.15.14: the remote light in the header (section 11): an arrow down for each request received, an arrow up for each reply sent |
| U | 5 November 2026 | Firmware 0.15.15: gain scheduling (section 10): three PID sets picked on the pressure or the setpoint against two boundaries, with hysteresis, Off by default; PID tuning gains a Schedule tab and, with the schedule on, a set row on the PID tab; the Home PID tile shows the set in use. The deadband splits into Deadband open and Deadband close and moves to Valve setup > Valve. Text port PIDS?, PIDS SET, SCHED?, SCHED SET; PID? and PID SET act on the set in use. Modbus map version 2: registers 26 to 50 |
| V | 5 November 2026 | Firmware 0.15.16: a tap on the greyed area round a popup (the dimmed page, the greyed strip, a greyed header icon) closes it as Back; the limit switch question and the saved position offer still need an answer |
| W | 6 November 2026 | Firmware 0.15.17: the remote light (section 11) holds each arrow lit until 3 s after its last message, a steady light while a client polls, not a flash per message |
| X | 6 November 2026 | Firmware 0.15.18: on-screen Help rewritten for a trained operator: terse, units in brackets; no change to the controls |
| Y | 6 November 2026 | Firmware 0.15.19: Calibrate sensor back on the Menu (Service row) in place of Diagnostics, and off Settings; Diagnostics on System beside Firmware |
| Z | 6 November 2026 | Firmware 0.15.20: the gain schedule is the PID table on screen (PID tuning > PID table tab: Off or On); the setpoint table is removed (SCHED SET SP refused, Modbus register 27 value 1 refused, saved settings with it load Off); Home and Settings PID tiles show KP, TI and KD of the set in use, no set number; the set-in-use dot sits just before the set name |
| AA | 6 November 2026 | Firmware 0.15.21: the Menu tile for Calibrate sensor reads Calibration, on one line |
| AB | 6 November 2026 | Firmware 0.15.22: no DHCP wording on the screens (System strip "Static IP address."); Settings: Review and save on the second row beside PID tuning, filled dark with white lettering |
| AC | 6 November 2026 | Firmware 0.15.23: Menu Setup row reads System, Settings; the Settings tile and Settings > Review and save are filled dark blue (the Start AUTO colour) with white lettering |
| AD | 6 November 2026 | Firmware 0.16.0: the OFF screen is brighter, a light grey power symbol and near-white "Double tap to turn on" on black |
| AE | 9 November 2026 | Firmware 0.16.1: a yellow Limit tag beside the Valve caption on Home while the limit switch is pressed |
| AF | 9 November 2026 | Firmware 0.16.2: the Menu is nine icon tiles without row captions (Trend, Events, Valve position; Valve settings, PID tuning, Calibration; System, Diagnostics, Review and save in dark blue), so every page is one tap away; the Settings page goes. Choose sensor goes: one sensor per board, the 15 psia Druck, and a replacement is calibrated in place; old saves with several sensors load as the one in use. Valve setup is renamed Valve settings (Travel, Homing and Drive tabs, saved numbers only) and its Help and Valve position's Help point at each other |
| AG | 9 November 2026 | Firmware 0.16.3: Valve position and PID tuning swap places on the Menu (PID tuning top right, Valve position in the middle beside Valve settings) |
| AH | 9 November 2026 | Firmware 0.16.4: the pressed limit switch shows as a yellow edge round Home's Valve tile (no Limit tag); W-4 comes 0.2 s after the switch reads pressed off the seat (was two reads 1 s apart), and the strip and the limit question appear within 0.1 s |
| AI | 9 November 2026 | Firmware 0.16.5: Motor override gets a Switch tab. Pass 10 to 20 %, off after every restart: while on and homed, between 10 and 20 % open the limit switch does not stop closing and raises no W-1, W-4 or F-4 (a valve that binds about 13 % trips the force switch there); below 10 % it stops as always. Home marks 10 to 20 % yellow on the valve bar while it is on |
| AJ | 10 November 2026 | Firmware 0.16.7: the Menu tile for the Calibrate sensor page reads Pressure setup (was Calibration; "Pressure settings" does not fit on one line). Since 0.16.6 a setpoint step draws as one straight vertical line in the Home sparkline and on Trend |
| AK | 10 November 2026 | Firmware 0.16.8: Motor override > Switch passes the limit switch between 5 and 25 % open (was 10 to 20 %: on the bench the switch was still pressed when AUTO closed to 10 %, and W-4 ended AUTO there); below 5 % it stops closing as always. Home marks 5 to 25 % yellow on the valve bar while it is on |
| AL | 10 November 2026 | Firmware 0.16.9: the reversal hold in AUTO is 0.2 mbar of error at the set's KP, at most 0.1 % of travel (was always 0.1 %); with the PID table on, the set is picked from the lower of the pressure and the setpoint, so going down the setpoint's set takes over at once (going up nothing changes) |
| AM | 11 November 2026 | Not issued. It was held for the command names, which came after revision AN as revision AO |
| AN | 11 November 2026 | Firmware 0.16.11: Motor override > Switch passes the limit switch between 5 and 50 % open (was 5 to 25 %); below 5 % it stops closing as always. Home marks 5 to 50 % yellow on the valve bar while it is on |
| AO | 11 November 2026 | Firmware 0.16.12: the text port command names are whole words. PRES? is PRESSURE?, PRES SP is SETPOINT, VALV is VALVE (VALVE?, VALVE MOVING?, VALVE SET), ALRM is ALARM (ALARM?, ALARM RESET, ALARM OK), EVNT? is EVENT?, BARO? is BAROMETER? and NETW? is NETWORK?; the replies start with the same words. MODE, STOP, TIME, PID, PIDS, SCHED and *IDN? are unchanged. The four-letter names answer ERR 100. ERR 206 and 207 say setpoint (was target) |
| AP | 11 November 2026 | Firmware 0.16.13: with the PID table on, a setpoint change that also changes the PID set moves the valve by the new set's KP times the change at once, as one within a set does. Before, the re-base on the set change took that step away and the valve moved on the integral alone (bench 11 Nov: 16 to 12 and 8 to 4 mbar) |
| AQ | 11 November 2026 | Firmware 0.16.14: a fourth PID set (section 10). Boundary 3 (B3, 14.0 mbar) splits the old set 3 range: set 3 from B2 to B3, set 4 above B3; B3 0 keeps three sets. Factory boundaries 6, 10 and 14 mbar (B2 was 20). The PID tab's set row has four sets; the PID table tab shows the two switches over B1, B2 and B3. Settings saved by 0.15.15 to 0.16.13 load with set 4 as set 3 and B3 0. Text port: PIDS? answers 13 numbers, PIDS SET takes set 4, SCHED? and SCHED SET carry B3 (SCHED SET PV,6,10,14,15). Modbus map version 3: set 4's KP, TI and KD at 51 to 56, B3 at 57 to 58 |
| AR | 12 November 2026 | Firmware 0.16.15: KP acts on the pressure only. A setpoint change, in a PID set or across sets, no longer moves the valve by KP times the change at once; the integral takes the valve to the new setpoint. Before, the valve jumped and gave most of the jump back as the pressure arrived, a bounce on the way in (simulator, 11 Nov: 16 to 12 mbar on 0.50/1.00). Steps take longer, by 10 to 40 % in the simulator |
| AS | 12 November 2026 | Firmware 0.16.16: the setpoint band (PID tuning > AUTO limits) can be set from 0 (was 0.10 mbar at least); with 0 the strip never reads In band or Stable, and W-2 still judges against 0.10 mbar, so a band under 0.10 does not end AUTO after 30 min. The Valve settings page is renamed Motor settings (same Travel, Homing and Drive tabs) |
| AT | 12 November 2026 | Firmware 0.16.17: Motor override moves from Valve position to Motor settings, on a fourth tab, Override (Open Motor override, dark outline, asks "Open Motor override?" first); Back from Motor override returns to that tab. Valve position keeps Home valve, Set as 0 %, Set as other %, Use last saved and Help |
| AU | 12 November 2026 | Firmware 0.16.18: factory settings are the PID table of the 12 Nov bench step tests: PID table On by the pressure, B1 6, B2 10, B3 14 mbar, hysteresis 15 %; set 1 KP 0.06 TI 0.60, set 2 0.12/0.82, set 3 0.80/0.94, set 4 0.40/0.25, KD 0 (was KP 4.00 TI 0.25 in every set, the table Off). Saved settings keep their own; Load settings > Factory gives these. Trend's pressure axis and the Home sparkline are at least 20 % of the highest pressure or target shown tall, 1 mbar at least (Trend was 0.5 mbar at least), so small wander round a setpoint looks small |
| AV | 12 November 2026 | Firmware 0.16.19: the switch pass band is a toggle on Motor settings > Override, Switch pass 5 to 50 %, under Open Motor override; Motor override has no Switch tab (Move, Speed and Coils). What the band does is unchanged; the Motor override strip still says when the switch is passed |
| AW | 12 November 2026 | Firmware 0.16.20: the PID table is always on; its Off choice (set 1 at every pressure) is removed from the PID table tab, the text port (`SCHED SET OFF` answers `ERR 102`) and Modbus (holding register 27 takes 2 only). Settings saved with the table Off load with it on. The autotune in the PC GUI puts the gains under test in all four sets instead of turning the table Off |
| AX | 12 November 2026 | Firmware 0.16.21: the switch pass is Ignore switch From to To %, with clearer wording on its card and in Help (Off: limit switch always stops closing; On: closing still stops below From; On: no effect until valve is homed). Its edges From and To are saved settings on the Override tab (5 and 50 % by default, From below To; fixed at 5 and 50 before), and the event reads Limit switch ignored from From to To % open: on or off. The Override tab has the toggle first, then From, To and the Motor override button. Drive is the first Motor settings tab (Drive, Travel, Homing, Override). The settings record is version 10; older saves load with the edges at 5 and 50 |
| AY | 13 November 2026 | Firmware 0.16.22: Ignore switch is Disable switch, and its card line is shorter (Off: switch stops closing; On: still stops below From; On: works once homed). From and To can be set from 0 to 100 % (From 0 to 99, To 1 to 100; was 1 to 99 and 2 to 100); with From 0 closing stops only at 0 % by the step count. The event reads Limit switch disabled from From to To % open. The Help pages (Motor settings, PID tuning, Review and save, Calibrate sensor, Valve position) and the Load settings line are shorter |
| AZ | 13 November 2026 | Firmware 0.16.23: Trend's pressure axis and the Home sparkline are at least 25 % of the highest pressure or target shown tall, 1.5 mbar at least (were 20 % and 1 mbar): 1.5 mbar at 2 and 5 mbar, 2.5 mbar at 10 mbar, 6.25 mbar at 25 mbar |
| BA | 13 November 2026 | Firmware 0.16.24: System is Cryostat name and Remote control on the first row, IP address and MAC address on the second, and Firmware, Restart and Shut down on the third. Shut down asks the power icon's question. The Diagnostics button is gone from System; Diagnostics stays on the Menu |
| BB | 13 November 2026 | Firmware 0.16.25: PID tuning reworked after a usability review. Tabs Gains, Set ranges and AUTO (were PID, PID table and AUTO limits). Gains: the set row is four cards, the one shown outlined, so it no longer looks like a second tab bar; the gain cards name the set (KP 2, TI 2, KD 2; KD in %s/mB, % per mB/s). Set ranges: Set 1 \| 2, Set 2 \| 3 and Set 3 \| 4 (boundaries 1 to 3) first, Hysteresis under them, and a line saying the lower of pressure and setpoint picks the set; boundary 3 of 0 shows None. AUTO: AUTO min and Pressure response, SP band under them with a line saying what it is; a band of 0 shows None. The help notes no longer say Save to keep |
| BC | 13 November 2026 | Firmware 0.16.26: the Home sparkline is at least 10 mbar tall, 5 mbar either side of the pressure (0 to 10 mbar at low pressures), so small wander and steady-state error do not show there and a big change does; Trend keeps 25 %, 1.5 mbar at least. The header's remote arrows are smaller (8 x 11 px each, were 10 x 16) |
| BD | 13 November 2026 | Firmware 0.16.27, after a usability review. Diagnostics: no row captions; cards from the left edge and Help at the top right as on the other pages; row 1 the transducer in signal order (Excitation, Bridge, Raw; Signal and Nominal before), row 2 the motor (Motor steps, Limit switch, Last move with how long it took, the Move took card gone), row 3 Room and Board. The Menu tile Pressure setup is Pressure sensor and its page Sensor calibration (Calibrate sensor before): row 1 Signal, Pressure (with the calibration in use) and Saved cal (In use when it is the one in use; Captured, which read None yet almost always, is gone); a point captured since the last save has the dark draft edge; the vacuum question reads "Sensor pumped down? Capture records 0 mB" with Enter value (was Not 0 mB); Help is two lines |
