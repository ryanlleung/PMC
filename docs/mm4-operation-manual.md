# mm4 Operation Manual

Revision A, 27 October 2026. Firmware 0.12.17.

This manual covers day-to-day use of the mm4 pressure controller (Option C) at firmware 0.12.17: power-up, homing, MANUAL and AUTO, calibration, faults and warnings, settings, and remote access over Ethernet. It is written for the operator at the cryostat and for bench users. Engineering commands that bypass the valve's checks are listed but not described step by step.

| Field | Value |
| --- | --- |
| Equipment | mm4 pressure-based needle-valve controller for helium-flow cryostats (CF-10, CF-11, HM-2, AVM-1, AVM-2) |
| Hardware | MikroE Mikromedia 4 for STM32F4 with Stepper 3, Boost 10 and Power Monitor Click boards; Druck UNIK 5000 15 psia transducer; geared unipolar stepper on the needle valve |
| Firmware | 0.12.17, branch claude/project-thread-w676ed |
| Sources | Firmware source and README; MM4 Design Description Rev S; Control Logic Rev O; Firmware Study Guide (0.12.16); mm4 Screens and Messages (0.11.36); Port 5000 and Modbus (0.12.7); docs/modbus-register-map.md (map version 1); bench-test notes for stages 0 to 8, Modbus and Ethernet |
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

What the operator does not do: tune the control loop. The gain and integral time are set once at commissioning (Settings > Control tuning) and are not part of normal operation.

The controller has four modes, shown in the header of every screen:

| Mode | Meaning |
| --- | --- |
| NOT HOMED | The valve position is not known. Nothing moves at power-up until the operator homes the valve or sets its position |
| HOMING | The valve is finding the closed limit switch |
| MANUAL | The operator positions the valve. The pressure is shown but not regulated |
| AUTO | The controller moves the valve once a second to hold the target pressure |

A red header with FAULT means a fault is latched (section 9).

The controller is operated from its 4.3 inch touchscreen. It can also be read and commanded over Ethernet: a text command link on TCP port 5000 for bench work and a Modbus TCP server on port 502 for a supervisory system (section 11). The USB-C port carries the same text link when the firmware is built for USB instead of Ethernet.

## 2. Safety and limits

The controller has no hardware interlock. The closing stop is the firmware reading the valve's limit switch before every closing step. This was a design decision for the prototype. The points below follow from it.

- **The limit switch can trip early.** It is a force-actuated normally closed contact. Ice on the valve, or a stiff drive, can press it before the needle reaches the seat. The controller cannot tell an early trip from the seat. When the switch trips during a normal move the controller stops and asks the operator (W-1, section 9). Only a person at the cryostat can judge which it was.
- **STOP** on the bottom row of every screen stops the valve at once and holds it. In AUTO it also switches to MANUAL. Nothing latches, so operation can resume straight away.
- **Nothing moves at power-up.** The valve is NOT HOMED until the operator homes it or sets its position. The controller never uses a saved position on its own.
- **Limit override** (Settings > Motor override) drives the valve closed past an open limit switch. It takes two fingers: hold Limit override, then press Close with another finger. Holding Limit override alone does nothing. The valve moves only while both are held and stops at once when either lifts. Open also works while it is held. Use it only when the switch is known to be stuck and the valve is known to be off the seat.
- **Motor supply.** The Stepper 3 Click has no current limit. The motor supply must be a regulated 12 V. Do not feed 12 V into the Boost 10 Click, which takes 5 V from the board and makes the 10 V Druck excitation.
- **Remote writes are on.** Anyone who can reach the controller's IP address on port 5000 or port 502 can set the target, move the valve in MANUAL, start AUTO and stop. Keep the controller on the instrument network only.

Operating limits:

| Item | Limit | Source |
| --- | --- | --- |
| Board supply | 12 V on the terminal (10.6 to 14 V) | Mikromedia 4 |
| Druck excitation | 9.5 to 10.5 V. Outside this is fault F-2 | Firmware |
| Pressure range | 0 to 1034 mbar absolute (15 psia sensor). Display resolution 0.1 mbar; accuracy set by the Druck (±0.04 % FS, ±0.4 mbar) and the calibration points | Druck datasheet |
| Target | 0.0 to the sensor range, kept to 0.1 mbar. Operators use 1 to 25 mbar | Firmware |
| Valve travel | 0 to 100 % of 66048 steps (4 output turns) by default. 0 % is the back-off point just above the seat; a seated valve reads about -0.6 % | Firmware 0.12.12 |
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
| Pressure 21 Click (BMP581) on Adapter Click, socket S1 | Room pressure for the air point of the calibration. Driver built, Click not yet fitted (not benched) | I2C, address 0x46 |
| On-board serial flash | Settings, calibration and the saved valve position. Survives reflashing | |
| On-board real-time clock | Time stamps for the event log and saves. Survives a reset, and a power-off with a cell fitted | |

Power: the board takes 12 V on its terminal, with USB-C and the Li-Po connector as lower-priority sources; SW1 must be on. The motor supply is a regulated 12 V from a PoE splitter. Both the board and the motor need their 12 V; the board alone runs the screens, the link and the pressure reading.

Earth: bond one 0 V point of the controller to the chassis earth. On the bench the board floating on the PoE splitter's 12 V put noise on the pressure reading. One 0 V to earth bond removed it (bench, 21 October 2026). This bond is not yet in the design drawings.

Reference gauge for calibration: a Pfeiffer TPG 202 Neo (USB-C readout only). The vacuum reading is typed into the controller by hand (section 8).

Socket pin details are in InitialTesting/clicks.h, limit.h and extflash.h, and the wiring drawings are in the project's wiring folder.

## 4. Power-up and first use

At power-up the controller loads the newest saved settings, starts the pressure reading and shows the Home screen in NOT HOMED. The valve does not move.

1. Check the header. It reads NOT HOMED. The Diagnostics page (Menu > System > Diagnostics) shows the firmware version, the board temperature and the link in use. The version should be 0.12.17.
2. Check the pressure. The Pressure tile shows the line pressure in mbar. Dashes mean no valid reading (section 9, F-1). If the strip says the nominal calibration is in use, the sensor has not been calibrated on this board (section 8).
3. If a saved position is on offer, the screen asks "Valve position not known. Saved position X %" with Home, Set position and Use X %. The saved position is the step count written before the last power-off. If power was lost during a move the prompt adds "may be out by E %". Use X % is quick but is not checked against the limit switch. Home is the checked option.
4. Home the valve (section 7). This takes up to a few minutes. The header then reads MANUAL and the strip reads "Valve at 0.00 %" (or close to it).
5. Set the valve where the cryostat needs it, in MANUAL, or start AUTO with a target (section 6).

After a restart the target is not kept, so the first Start AUTO asks for it again. The settings, calibration and tuning saved with Save settings are kept. The trend and the event log are in RAM and start empty.

The real-time clock keeps time through a reset. If the dates on saves and events are wrong, set the clock over the text link with the TIME SET command (section 11). There is no clock page on the screen.

First use of a new board or a new Druck sensor also needs: the sensor added under Settings > Choose sensor with its serial number and range, a two-point calibration (section 8), the valve travel and motor direction checked under Settings > Valve setup, the pressure response direction set under Settings > Control tuning, and Save settings. These are commissioning steps, done once.

## 5. Screens

Every screen has the same frame: a header with Back and Home buttons, the title, and the mode badge on the right; a status strip; and STOP on the bottom row. The strip shows the valve's state (Valve at X %, Moving to X %, or in AUTO Regulating to, In band or Stable at the target) with the 5 minute pressure sparkline, and takes over for prompts, faults and messages. Under a popup the strip stays visible and greyed. Colour follows the ISA-101 convention: grey when normal, colour only for an abnormal state.

| Screen | How to reach it | What it shows and does |
| --- | --- | --- |
| Home | Home button | Pressure tile (reading in mbar, log bar 0.1 to 1000 mbar, Target box) and Valve tile (reading in %, bar, Go to box). The mode button reads Home valve, Start AUTO or Switch to MANUAL as the mode allows. Valve and Menu buttons. A tap on the sparkline opens Trend |
| Valve | Home > Valve | MANUAL moves: hold Close or Open, -1 %, +1 %, -5 %, +5 %, Fully close, Fully open, and the Go to box for a typed % (white with a blue underline when it takes a tap). In AUTO the buttons are grey and Go to shows where AUTO is sending the valve; a tap asks to switch to MANUAL |
| Menu | Home > Menu | Records: Trend, Faults and log. Calibrate: Valve position, Transducer. System: Diagnostics, Settings |
| Trend | Menu > Records > Trend | Pressure (left axis), the target while in AUTO (dashed) and valve % (right axis) over 1, 2, 5, 10, 30 min or 1 hour |
| Faults and log | Menu > Records > Faults and log | Latched faults, with Reset fault. Tap a fault for its detail and help. The event log (last 100 events) |
| Valve position | Menu > Calibrate > Valve position | Home valve, and the three ways to set the position without homing: Set as 0 %, Set as other %, Use last saved. Help |
| Calibrate sensor | Menu > Calibrate > Transducer | The two-point calibration: Capture vacuum, Capture air, and the saved and captured readings side by side |
| Diagnostics | Menu > System > Diagnostics | Live readings: Druck signal, pressure before calibration, the room sensor (Pressure 21), motor steps, limit switch and last move, board temperature, excitation and how long the last move took |
| Settings | Menu > System > Settings | System, Choose sensor, Valve setup, Control tuning, Review and save (with Load settings), Motor override (section 10) |

Display formats: pressure to 1 decimal in mbar, valve position and keypad entries to 2 decimals in %, dates DD/MM/YY.

Keypads: a tap on a value box opens a number pad with the allowed range under the title. Set applies the value. A value outside the range is refused with the range shown. Cancel leaves the old value.

Short messages: a refused action puts its reason in the strip for a few seconds, for example "Switch to MANUAL first", "Home the valve first", "Reset the fault first", "Limit switch pressed, cannot close" or "Wait for the valve to stop".

## 6. MANUAL and AUTO, setting the target

### MANUAL

In MANUAL the operator positions the valve and watches the pressure. Position is in percent of full travel: 0 % is just above the seat, 100 % is 4 output turns open by default.

- On the Valve page, hold Close or Open to move while pressed, or tap -1 %, +1 %, -5 % or +5 % to step from where the valve is going.
- Tap the Go to box (on Home or the Valve page) to type a position, then Set. Fully close and Fully open go to 0 % and 100 %.
- Moves run at the drive's run speed (Settings > Valve setup > Drive). A new Go to during a move retargets it: further on extends the move, behind it ramps down and turns round.
- The limit switch stops any closing move. If it trips away from 0 % the controller asks Set as 0 % or Skip (W-1, section 9).
- Before homing, Close, Open and the step buttons still move the valve by steps, but Go to, Fully close and Fully open need the valve homed.

### Starting AUTO

AUTO needs the valve homed (or its position set), no fault latched, a valid pressure reading and a target. Press Start AUTO on Home.

- The first time after power-up it opens "Target for AUTO, mB" with the allowed range. Type the target and press Set. AUTO starts at once.
- Later presses ask "Start AUTO at X mB?" with Cancel, New target and Start AUTO.
- A refused start says why in the strip: Home the valve first, Reset the fault first, No valid pressure, Set a target first, Target outside the sensor range, Pressure above the sensor range, Pressure response not set, Tuning not set, or Limit switch pressed away from the seat.

The entry is bumpless: the loop starts from where the valve is, so the first move is small.

### In AUTO

Once a second, after the pressure reading, the controller works out a new valve position and moves to it. The strip reads Regulating to X mB, then In band, X mB when the pressure is within the target band (0.1 mbar by default), then Stable at X mB after 60 s in the band. The Trend page shows the target as a dashed line.

- Change the target from the Target box on Home. A change of more than 5 mbar asks "Change target to X mB?" first. Smaller changes apply at once.
- The valve moves in short bursts rather than continuously. Small reversals are held until the command is at least 0.1 % of travel from the valve, so the reading's noise does not turn the motor round inside the drive's backlash (firmware 0.12.12). The motion is still less smooth than the Mercury iTC's; this is a known difference (26 October 2026).
- The Valve page is locked: the move buttons are grey and a tap on Go to asks to switch to MANUAL. Homing, valve settings and Save settings are refused in AUTO with "Switch to MANUAL first".
- The controller watches for a pressure that does not respond to valve moves (F-6), a pressure that does not settle in 30 min (W-2), a pressure above the sensor range (W-5) and the valve pinned at an end the wrong way (W-6). Each of these ends AUTO; section 9 says what to do.

### Leaving AUTO

- Switch to MANUAL on Home asks "Switch to MANUAL? Valve holds where it is." The valve ramps down if it was moving and holds. The target is kept for the next Start AUTO.
- STOP does the same at once, without a question.
- A fault, W-2, W-4, W-5, W-6, or the motor supply dropping out also end AUTO. The valve holds where it stopped and the strip says why.

When AUTO ends for any reason the valve stays where it was. It does not close. If the cryostat needs the valve elsewhere, move it in MANUAL.

## 7. Homing the valve

Homing finds the closed limit switch and sets 0 % from it. It is the only way the controller checks the valve position. Home after every power-up, after a motor direction or coil order change, and whenever the position is in doubt.

Start it from Home valve on the Home screen (while NOT HOMED) or from Menu > Calibrate > Valve position. The question "Home the valve? It closes onto the limit switch to find 0 %, up to N min" confirms it. The Valve position page shows the step running, the motor step count and the time taken.

1. If the switch is already pressed, the valve moves off it first. If the switch stays pressed, fault F-4.
2. Fast close to the switch. If the switch is not found within 1.1 times the full travel, fault F-3.
3. Back off the switch by the back-off distance (400 full steps by default).
4. Slow close onto the switch. This point is the seat.
5. Back off again by the same distance, with the switch released. This last point is 0 %. A seated valve therefore reads about -0.6 %.

The strip reads "Homing 1 of 3: fast close to the switch", "Homing 2 of 3: backing off the switch" and "Homing 3 of 3: slow close onto the switch" as it goes. STOP cancels homing and keeps the earlier homing, if there was one.

### Setting the position without homing

The Valve position page also offers three ways to set the position without moving the valve. None of them is checked against the limit switch, and the strip shows "X %, set by hand" or "X %, from saved" afterwards.

| Option | When to use it |
| --- | --- |
| Set as 0 % | Only when the valve is known to be seated. It then reads the seat, about -0.6 %, as after homing |
| Set as other % | When the position is known, for example from the previous run |
| Use last saved | The position saved before power was lost. Offered after start-up until the valve moves or is homed. If power was lost during a move the prompt says how far out it may be (at most about 2 s of travel) |

Home the valve when you can. Only homing checks the position.

### Limit switch during a move

If the switch trips during a normal closing move away from 0 %, the valve stops and the strip asks "Limit switch at X %. Set as 0 %?" with Skip and Set as 0 % (W-1). The count is never zeroed on its own.

- Set as 0 % when the valve really is at the seat, for example after a direction change or a slipped drive.
- Skip when the switch tripped early (ice, a stiff drive). The count is kept. Open the valve, let the ice clear, and home again later.

A switch that reads pressed with the valve well off the seat raises W-4 (section 9).

## 8. Sensor calibration

The Druck output is ratiometric, so the controller reads the ratio of signal to excitation in mV/V and turns it into pressure with a two-point calibration: a vacuum point and an air point. Both points are read live through the board as wired, so the board's own loading, gain and offset sit inside the calibration. With no calibration saved the nominal 0 to 100 mV at 10 V is used and the screen says so; readings are then about 5 % low.

Calibrate when a sensor is first fitted to a board, when the sensor or the Click boards are changed, and when a check against the reference gauge shows drift. Calibration is refused in AUTO.

From the screen (Menu > Calibrate > Transducer):

1. Pump the sensor down. Press Capture vacuum. The question "Sensor at vacuum? Capture sets 0 mB" has Capture, which takes the point as 0 mbar, and Not 0 mB, which opens a keypad for the reference gauge reading (0 to 50 mbar). Type what the Pfeiffer gauge shows. The capture averages 5 readings and takes about 5 s.
2. Vent the sensor to the room. Press Capture air. With the Pressure 21 room sensor fitted, the question "Sensor open to room air?" shows its reading: Capture takes the air point at the room sensor's reading averaged over the same 5 readings, and Enter value opens a keypad for a laboratory barometer reading instead. Without the room sensor the keypad opens straight away: type the barometer reading in mbar, for example 1013.2 (500 to 1100 mbar).
3. Once both points are held the new calibration is in use and unsaved. Compare Saved cal against Captured on the page.
4. Press Save settings (Settings > Review and save). Discard changes puts the last save back.

Either point can be redone on its own; the other is kept.

Over the text link the same calibration is CAL VAC (with an optional gauge reading in mbar), CAL AIR followed by the barometer reading (or CAL AIR alone to use the room sensor), CAL? to check, then CAL SAVE. CAL ERASE goes back to nominal and saves. A calibration table of up to 32 points can also be typed in with CAL NEW, CAL PT and CAL APPLY.

Up to four sensors can be kept on one board, each with its own calibration, under Settings > Choose sensor. Add a sensor with its serial number and range in psi, then calibrate it. Only the sensor in use is read.

The Pressure 21 Click in S1 gives the controller its own room-pressure reading (BARO?). It is used for the air point. The Click has not arrived and this has not been benched.

## 9. Faults and warnings

Faults latch until Reset fault (on the strip, on Faults and log, or in the fault detail), which clears only those whose cause has gone. The strip shows the highest-priority fault; the number order is the priority order. Any fault in AUTO stops the valve and switches to MANUAL. F-1 and F-2 are sensor faults and do not block valve moves, so the valve can still be moved and homed with no pressure reading. F-3 to F-6 block moves until reset. Tap a fault on Faults and log for its detail: what happened, what the controller did, what to check, and when it can be reset.

| Fault | Cause | What the controller did | Check | Reset |
| --- | --- | --- | --- | --- |
| F-1 No pressure signal | The Druck reading is missing or outside 0.005 to 11 mV/V | Pressure shows dashes. The valve still moves. AUTO ends | The Druck cable and plug, and the Power Monitor Click | When the reading is back |
| F-2 Excitation | The Druck excitation left 9.5 to 10.5 V, or the Boost 10 tripped | Pressure not trusted. The valve still moves. AUTO ends | The Boost 10 and the board's 5 V. A trip needs a power cycle | When the excitation is back in the window |
| F-3 Limit not found | Homing closed 1.1 times the full travel without the limit switch | Stopped. The valve is not homed | The switch and its wire, and that the valve turns | Any time. Then home again |
| F-4 Limit stuck | The limit switch stayed pressed after the valve moved off it | Stopped. The valve is not homed | The switch is not stuck (ice) and its wire is not broken | Any time. Then home again |
| F-5 Motion timeout | A move took longer than its distance at the set rate plus the ramps plus 2 s | Stopped the valve and holds it. Homing kept | The valve turns freely, and the motor 12 V supply | Any time |
| F-6 No response | The valve moved in AUTO but the pressure did not respond over 5 windows of 30 s | Stopped the valve and holds it. MANUAL | Drive slipping, motor unplugged, needle seized, helium supply lost | Any time. Check before starting AUTO again |

Warnings do not latch. W-2, W-4, W-5 and W-6 show on the strip until OK is pressed or the next Start AUTO, and each ends AUTO. W-1 clears when its question is answered. W-3 clears on its own.

| Warning | Cause | What the controller did | What to do |
| --- | --- | --- | --- |
| W-1 Limit switch pressed at X % | The switch tripped during a closing move away from 0 % | Stopped the valve and asks Set as 0 % or Skip, over any screen | Set as 0 % if the valve is at the seat. Skip if the switch tripped early (ice). Section 7 |
| W-2 Not settling | 1800 s outside the target band in AUTO | AUTO ended, MANUAL, valve idle where it stopped | Check the pump, the helium supply and the target against what the cryostat can reach. Press OK. Start AUTO again when ready |
| W-3 Controller hot | Power Monitor die at or above 55 °C | Nothing blocked. Shown until it cools below 50 °C | Check the enclosure ventilation |
| W-4 Switch pressed off the seat | The switch reads pressed with the valve well off the seat, two reads in a row | In AUTO: AUTO ended, valve idle. In MANUAL: a strip warning | Check the switch, its wiring and the valve position. Home again when the switch is free |
| W-5 Over range in AUTO | The pressure is above the sensor range, two reads in a row | AUTO ended, MANUAL, valve idle | The line is at or near atmosphere. Pump down, then start AUTO |
| W-6 Driven the wrong way | The valve sat at an end of travel for 60 s with the pressure moving away from the target | AUTO ended, valve idle | The pressure response setting is the wrong way round for this valve. Check Settings > Control tuning > Pressure response |

Other strip messages:

- **Limit override on**, with Cancel: the override from Settings > Motor override is active.
- **Valve not homed**: the valve position is not known.

The event log (Menu > Records > Faults and log) keeps the last 100 events with a time stamp: faults, warnings, mode changes, moves, settings saves, calibration captures and remote commands with the client's IP address. It is in RAM and empties at a restart.

## 10. Settings

Settings are commissioning items, not daily operation. Edits on the screen are drafts until Save settings, which checks them together and saves all or nothing. Changes made over the text link are live at once and unsaved until CFG SAVE. Save settings and CFG SAVE are refused in AUTO. The board keeps the last two saves, in two flash sectors in turn, and reads a save back before it counts.

| Page | Settings | Notes |
| --- | --- | --- |
| System | Cryostat name (up to 12 characters, shown in the Home title). Fallback IP address and mask. Remote control switch (On by default). Firmware version and build date | The name is in use at once and unsaved until Save settings. The fallback address is used when no DHCP server answers, after Save settings and a restart. Default puts the build's address back. The page shows the address in use |
| Choose sensor | Up to four Druck sensors, each with serial number, range in psi and its own calibration. Sensor in use | A new sensor starts uncalibrated. Calibrate puts it in use and opens Calibrate sensor |
| Valve setup, Valve tab | 100 % travel in output turns (4 by default, 16512 steps per turn). Motor direction | Changing the direction clears the homing. Home again after saving |
| Valve setup, Homing tab | Back-off (400 full steps), fast close speed (400 steps/s), slow close speed (25 steps/s) | Homing step distances and speeds |
| Valve setup, Drive tab | Run speed (300 steps/s), start speed, acceleration | Used by MANUAL and AUTO moves. Saved when the valve is stopped |
| Control tuning | Gain (% open per mbar, 4.00), integral time (min, 0.25; 0 is off), AUTO min (%), deadband (steps, 8), pressure response (opening raises or lowers the pressure), target band (mbar, 0.10) | Set at commissioning. Operators do not change these. A wrong pressure response gives W-6 |
| Review and save | Last save and previous save by date. Unsaved changes list. Save settings. Discard changes. Load settings: last save, previous save or factory defaults | A loaded save is in use at once, sensors and calibrations included. Factory defaults come in as drafts and keep the sensors. Load is refused while the valve moves or in AUTO |
| Motor override | Move tab: step size 1 to 10 000 or Hold, Close and Open, Zero, Limit override. Speed tab: run, start and acceleration rates. Coils tab: step mode (wave, full, half), hold torque, coils off, coil order, direction | Engineering page. It drives the motor by steps past the valve's checks. Coil order, direction and Zero clear the homing and ask first. The strip reads the motor state and the limit switch |

Factory defaults for the valve and the loop are for the PK22 344:1 stepper on the Oxford Instruments needle valve. The CF-11 on the bench ran at about 1.6 K with 3 to 8 mbar, the valve around 34 % open, on the Mercury iTC's P 20 % and I 0.1 min. The mm4's equivalent gain and integral time for each cryostat are set at commissioning.

The step test (TUNE STEP over the text link) moves the valve in MANUAL, records the pressure response and suggests a gain and integral time. It applies nothing. It has not been run on a cryostat (not benched).

## 11. Remote access

The Ethernet build (the one for the cryostat) runs two servers on one cable and one IP address. The USB build has the text link on the USB-C port instead and no Modbus. The two cannot run together.

### Network and IP address

The controller asks for an address by DHCP. If no DHCP server answers within 10 s it uses the fallback address: the one saved under Settings > System setup, or the build's default 169.254.10.50/16. The header shows the address in use, or NO CABLE, DHCP..., NET NO CLK or NET NO PHY while it has none. Diagnostics shows the link and the address. The hostname is pmc.

To set the fallback address, type it and the mask on the System page (or NET IP over USB), then Save settings and restart. NET IP is refused over port 5000 so a remote user cannot cut off their own link. On a laptop with a fixed address, put the laptop and the fallback address on the same subnet.

Ethernet and Modbus were proven on the bench on 26 October 2026 with the laptop on a fixed address.

### Port 5000: text commands

Port 5000 takes every command the USB link takes, in plain text, one per line, for a person at a terminal (PuTTY in Raw mode, or a Git Bash /dev/tcp redirect). The board echoes each line, answers data lines that start with the group word, and ends every command with OK or ERR and a reason. Anything that finishes later arrives as an EVT line. Type HELP for the groups and HELP CTRL, VALVE, TUNE, MOT, CAL, CFG, DATA or SYS for one group.

| Group | Commands an operator or bench user needs |
| --- | --- |
| Reading | VALVE? mode, position, faults and any waiting switch question. CTRL? latched faults. SENSE? the live sensor readings. EVT? the last events. DATA ON starts a once-a-second DATA line (time, pressure, target, valve %, mode, alarm, ratio, excitation, temperature); DATA OFF stops it. DATA? lists the columns |
| Control | CTRL SP followed by the target in mbar. CTRL AUTO starts AUTO with the same checks as the screen. CTRL MAN switches to MANUAL, with an optional % to go to. CTRL RESET clears faults whose cause has gone. CTRL OK acknowledges a strip warning |
| Valve | VALVE CAL homes. VALVE GOTO followed by a % moves in MANUAL. VALVE STOP is the one stop command: it stops the valve and ends AUTO. VALVE SETZERO and VALVE SKIP answer the switch question. VALVE SAVED? and VALVE RESTORE read and use the saved position at start-up |
| Calibration | CAL?, CAL VAC, CAL AIR, CAL SAVE, CAL ERASE (section 8) |
| Settings | CFG? and CFG DIFF? show the saves and unsaved changes. CFG SAVE saves. CFG LOAD LAST, PREVIOUS or DEFAULTS loads. CFG NAME sets the cryostat name. NET? and NET IP for the fallback address |
| System | VER? firmware version, build time and last reset cause. *IDN? identity. TIME? and TIME SET to read and set the clock. LIM? limit switch. BARO? the Pressure 21 (not fitted). PERF? and TOUCH? diagnostics |
| Motor (engineering) | MOT? and the MOT MOVE, GOTO, STOP, OFF, ZERO, OVERRIDE, RATE, START, ACCEL, MODE, HOLD, DIR and ORDER commands drive the motor directly, past the valve's checks. MOT STOP and MOT OFF end AUTO first |
| Tuning (engineering) | PID? and PID KP, TI, OUTMIN, DB, BAND and KDIR set the loop live. TUNE STEP, TUNE? and TUNE STOP run the step test |

In AUTO, moves, homing, calibration, settings loads and saves answer "Switch to MANUAL first". The full command table is in the README.

Tools in the repository: tools/logger/log_druck.py logs the DATA lines to CSV from an IP address or COM port and reconnects on its own. tools/cal/druck_cal.py manages the calibration table.

### Port 502: Modbus TCP

The Modbus server is for a supervisory system. It reads 26 holding registers (0 to 25) and 14 coils, and writes only the target, the valve position in MANUAL, and the AUTO, STOP, RESET and warning OK coils. Homing, the switch question, calibration, settings and tuning are not on Modbus, because each needs a person watching or changes what the controller trusts.

- Function codes 1, 3, 5 and 16. Up to 2 clients at once. Unit id not checked. Addresses are 0-based.
- Floats are IEEE 754 single precision in two registers, high word first. NaN means no value.
- Values update once a second, so polling faster than 1 Hz adds nothing. Read registers 0 to 25 in one request.
- Register 1 is a heartbeat (seconds since start-up). A value that stops changing means the controller has stopped.

| Address | Content |
| --- | --- |
| Register 2 | Mode: 0 NOT HOMED, 1 HOMING, 2 MANUAL, 3 AUTO |
| Register 3 | Status bits: reading valid, position set, homed, moving, switch pressed, fault, warning, target set, writes on, motor supply on |
| Register 4 | Regulation in AUTO: 1 Regulating, 2 In band, 3 Stable |
| Registers 5, 6 | Faults latched (bit 0 F-1 to bit 5 F-6) and warnings showing (bit 0 W-1 to bit 5 W-6) |
| Registers 10 to 11 | Pressure, mbar |
| Registers 12 to 13 | Target, mbar (writable, any mode) |
| Registers 14 to 15 | Valve position, % |
| Registers 16 to 17 | Valve going to, % (writable, MANUAL only) |
| Registers 20 to 23 | Excitation V and controller temperature degC |
| Coils 0 to 3 | AUTO (1 starts, 0 switches to MANUAL), STOP, RESET, warning OK |

A write the controller refuses answers exception 4 and logs the reason as a NET event, with the client's IP address. Exception 1 means the function code is not supported or writes are off; 2 an address outside the map or a read-only register; 3 a bad value or count. The complete map with every bit is docs/modbus-register-map.md.

The Modbus read test passed on the bench on 26 October 2026. The write test has not been run (not benched). What the controller should do if the supervisory system stops polling is still to be agreed with its owner; today it does nothing.

## 12. Maintenance

| Task | When | How |
| --- | --- | --- |
| Check the pressure reading against the reference gauge | At each cool-down, and whenever the reading looks wrong | Compare the controller's reading with the Pfeiffer gauge at vacuum and at air. Recalibrate (section 8) if either point is off by more than the sensor's accuracy |
| Home the valve | After every power-up, after a direction or coil order change, and when the position is in doubt | Section 7 |
| Check the limit switch | When F-3, F-4 or W-4 appears, and at each warm-up | LIM? over the link or the Valve position page shows pressed or free. Press the switch by hand and watch it change. Look for ice on the switch and the valve drive |
| Check the Druck excitation | When F-2 appears | Diagnostics or SENSE? shows the excitation. It should be 9.5 to 10.5 V. A Boost 10 trip needs a power cycle |
| Check the motor supply | When moves are slow, F-5 appears or the motor is noisy | Measure the 12 V at the Stepper 3 Click. The controller does not measure it |
| Review the event log | After a fault, and before clearing it | Menu > Records > Faults and log, or EVT? over the link. The log empties at a restart, so read it first |
| Save the settings | After any commissioning change | Settings > Review and save > Save settings, or CFG SAVE. Review and save shows the date of the last save |
| Set the clock | When the dates on saves and events are wrong | TIME SET over the link, in the form TIME SET 2026-10-27 09:00:00 |
| Update the firmware | When a new version is released | Flash over USB-C with mikroBootloader. The settings, calibration and saved position are in serial flash and survive reflashing. Check VER? or Diagnostics afterwards |
| Clean the touchscreen | As needed | Switch off first, or the taps will act on the screen. A soft cloth, no solvents |

The controller has no fuse, battery or filter to replace. The real-time clock runs from the board supply; a cell in the BAT1 holder keeps it through a power-off.

If the controller stops responding, the independent watchdog restarts it within about 4 s. The reset cause is shown in red on Diagnostics after a watchdog or brown-out reset, and VER? reports every cause. The valve does not move after the restart until it is homed or its position is set, and a position saved before the reset is offered.

## 13. Items not yet benched

The firmware is a prototype. The items below are in the firmware or the design but have not been confirmed on the bench as of 27 October 2026. Treat the manual's description of each as the intended behaviour.

| Item | State |
| --- | --- |
| Pressure 21 Click (room pressure, BARO?) and the air calibration reference from it | Driver built in 0.11.47. Click ordered 15 October, not arrived |
| Step test (TUNE STEP) | Built in 0.12.7 and 0.12.10 and run on a PC model. Not run on a cryostat |
| Modbus writes (target, valve going to, coils 0 to 3) | On since 0.12.6. The read test passed 26 October; the write test has not been run |
| VALVE STOP as the only stop command (CTRL STOP removed), MOT STOP and MOT OFF ending AUTO, CFG SAVE refused in AUTO, CTRL OK | Firmware 0.12.16 and 0.12.17, built 26 October. Passed the simulator tests; not yet flashed and checked on the board |
| AUTO on a cryostat with the production gain and integral time | AUTO has run on the bench rig against a manual vent valve, and its motion was compared with the Mercury iTC on 26 October. Tuning for each cryostat is still to be set |
| W-5 Over range and W-6 Driven the wrong way | Checked on the PC control-loop test, not provoked on the bench |
| Loss of polling from the supervisory system | No behaviour defined yet. Waiting on the owner of that system |
| Earth bond in the enclosure | Found on the bench 21 October; not yet in the enclosure drawings |

## 14. Revision history

| Revision | Date | Change |
| --- | --- | --- |
| A | 27 October 2026 | First issue, from firmware 0.12.17 and the design documents listed in the reference block |
