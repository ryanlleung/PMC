# iPC screens

One capture of each screen at firmware 0.18.0. The screens are 480x272; the captures are at twice that size. They are taken on the simulator, which runs the firmware's own screen code (`simulator/README.md`). `docs/operation-manual.md`, section 5, describes each screen.

| Screen | File | Shows |
| --- | --- | --- |
| OFF | [`off.png`](off.png) | Every power-up and restart starts here. Two taps turn the controller on |
| Turning on | [`turning-on.png`](turning-on.png) | The logo and bar for about 1.5 s; the firmware version and build date are in the corner |
| Home, not homed | [`home-not-homed.png`](home-not-homed.png) | After power-up without a kept position: Home valve is in the Valve tile and Start AUTO is greyed |
| Start-up question | [`startup-question.png`](startup-question.png) | A saved valve position is offered: Home valve, Set as % or use it |
| Home, MANUAL | [`home-manual.png`](home-manual.png) | Pressure and Valve tiles, the PID and IP tiles, Menu and STOP; the strip shows the status line and the 5 min sparkline |
| Home, AUTO | [`home-auto.png`](home-auto.png) | The SP box and Stop AUTO; the strip reads Regulating, In band or Stable |
| Keypad | [`keypad.png`](keypad.png) | Every number is typed on this keypad, over the dimmed page |
| Power options | [`power-options.png`](power-options.png) | The power icon on Home: Cancel, Restart or Shut down, with the close-the-valve checkbox |
| Closing valve | [`closing-valve.png`](closing-valve.png) | Shut down moving the valve to 0 %; STOP cancels |
| Menu | [`menu.png`](menu.png) | Nine tiles; every page is one tap away |
| Trend | [`trend.png`](trend.png) | Pressure, setpoint (dashed) and valve; Range from 1 min to 1 hour |
| Events | [`events.png`](events.png) | Latched faults on top, then the last 100 events, newest first |
| PID > Gains | [`pid-gains.png`](pid-gains.png) | KP, TI and KD of the zone shown; the zone in use is underlined |
| PID > Zones | [`pid-zones.png`](pid-zones.png) | Where zones 2, 3 and 4 start |
| PID > Settings | [`pid-settings.png`](pid-settings.png) | AUTO min, SP band, Hysteresis and Deadband |
| Help | [`pid-help.png`](pid-help.png) | Each settings page has a Help page |
| Valve > Position | [`valve-position.png`](valve-position.png) | Position, Travel, when it was homed, Set as % and Home valve |
| Valve > Homing | [`valve-homing.png`](valve-homing.png) | Homing speeds and distances |
| Valve > Limit | [`valve-limit.png`](valve-limit.png) | The Disable switch range and toggle |
| Motor > Jog | [`motor-jog.png`](motor-jog.png) | Close and Open, hold to run; Limit override is the second finger |
| Motor > Settings | [`motor-settings.png`](motor-settings.png) | Speeds, acceleration, steps per turn and direction |
| Pressure sensor | [`pressure-sensor.png`](pressure-sensor.png) | The reading, the signal and the two calibration points |
| Capture question | [`capture-question.png`](capture-question.png) | Asked before a calibration point is captured |
| System | [`system.png`](system.png) | IP address, MAC address, clock, Network control, firmware, up time and Power |
| IP address and mask | [`ip-address.png`](ip-address.png) | Address, Mask, Save and Default |
| Diagnostics | [`diagnostics.png`](diagnostics.png) | Raw readings of the sensor, the motor and the board |
| Fault F-1 | [`fault-f1.png`](fault-f1.png) | A latched fault: FAULT in the header, the tile edged red, Reset fault on the strip |
| Warning W-1 | [`limit-switch-question.png`](limit-switch-question.png) | The limit switch question: Skip or Set as closed |
| Warning W-2 | [`warning-w2.png`](warning-w2.png) | No pressure response: AUTO has ended, OK on the strip |
| Unsaved changes | [`unsaved-changes.png`](unsaved-changes.png) | The yellow save icon asks to save the settings in use |
| Factory reset | [`factory-reset.png`](factory-reset.png) | Asked before the motor, valve and PID settings are reset |

To take them again after a screen change, run `node simulator/tests/run.js --png <folder>`, which saves a capture at each step of the simulator tests.
