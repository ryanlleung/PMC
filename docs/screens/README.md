# iPC screens

One capture of each screen at firmware 0.18.0.

- The screen is 480 x 272 pixels. The captures are twice that size.
- The captures are taken on the simulator, which runs the firmware's own screen code. See [`simulator/README.md`](../../simulator/README.md).
- [`docs/operation-manual.md`](../operation-manual.md), section 5, describes each screen.

Click a picture to open it at full size.

## Contents

- [Power](#power)
- [Home](#home)
- [Menu, Trend and Events](#menu-trend-and-events)
- [PID](#pid)
- [Valve and Motor](#valve-and-motor)
- [Pressure sensor, System and Diagnostics](#pressure-sensor-system-and-diagnostics)
- [Faults, warnings and questions](#faults-warnings-and-questions)
- [Taking the captures again](#taking-the-captures-again)

## Power

| Screen | Capture | Shows |
| --- | --- | --- |
| **OFF**<br>`off.png` | [<img src="off.png" width="240" alt="OFF">](off.png) | Every power-up and restart starts here. Two taps turn the controller on |
| **Turning on**<br>`turning-on.png` | [<img src="turning-on.png" width="240" alt="Turning on">](turning-on.png) | The logo and bar for about 1.5 s; the firmware version and build date are in the corner |
| **Power options**<br>`power-options.png` | [<img src="power-options.png" width="240" alt="Power options">](power-options.png) | The power icon on Home: Cancel, Restart or Shut down, with the close-the-valve checkbox |
| **Closing valve**<br>`closing-valve.png` | [<img src="closing-valve.png" width="240" alt="Closing valve">](closing-valve.png) | Shut down moving the valve to 0 %; STOP cancels |

## Home

| Screen | Capture | Shows |
| --- | --- | --- |
| **Home, not homed**<br>`home-not-homed.png` | [<img src="home-not-homed.png" width="240" alt="Home, not homed">](home-not-homed.png) | After power-up without a kept position: Home valve is in the Valve tile and Start AUTO is greyed |
| **Start-up question**<br>`startup-question.png` | [<img src="startup-question.png" width="240" alt="Start-up question">](startup-question.png) | A saved valve position is offered: Home valve, Set as % or use it |
| **Home, MANUAL**<br>`home-manual.png` | [<img src="home-manual.png" width="240" alt="Home, MANUAL">](home-manual.png) | Pressure and Valve tiles, the PID and IP tiles, Menu and STOP; the strip shows the status line and the 5 min sparkline |
| **Home, AUTO**<br>`home-auto.png` | [<img src="home-auto.png" width="240" alt="Home, AUTO">](home-auto.png) | The SP box and Stop AUTO; the strip reads Regulating, In band or Stable |
| **Keypad**<br>`keypad.png` | [<img src="keypad.png" width="240" alt="Keypad">](keypad.png) | Every number is typed on this keypad, over the dimmed page |

## Menu, Trend and Events

| Screen | Capture | Shows |
| --- | --- | --- |
| **Menu**<br>`menu.png` | [<img src="menu.png" width="240" alt="Menu">](menu.png) | Nine tiles; every page is one tap away |
| **Trend**<br>`trend.png` | [<img src="trend.png" width="240" alt="Trend">](trend.png) | Pressure, setpoint (dashed) and valve; Range from 1 min to 1 hour |
| **Events**<br>`events.png` | [<img src="events.png" width="240" alt="Events">](events.png) | Latched faults on top, then the last 100 events, newest first |

## PID

| Screen | Capture | Shows |
| --- | --- | --- |
| **PID > Gains**<br>`pid-gains.png` | [<img src="pid-gains.png" width="240" alt="PID > Gains">](pid-gains.png) | KP, TI and KD of the zone shown; the zone in use is underlined |
| **PID > Zones**<br>`pid-zones.png` | [<img src="pid-zones.png" width="240" alt="PID > Zones">](pid-zones.png) | Where zones 2, 3 and 4 start |
| **PID > Settings**<br>`pid-settings.png` | [<img src="pid-settings.png" width="240" alt="PID > Settings">](pid-settings.png) | AUTO min, SP band, Hysteresis and Deadband |
| **Help**<br>`pid-help.png` | [<img src="pid-help.png" width="240" alt="Help">](pid-help.png) | Each settings page has a Help page |

## Valve and Motor

| Screen | Capture | Shows |
| --- | --- | --- |
| **Valve > Position**<br>`valve-position.png` | [<img src="valve-position.png" width="240" alt="Valve > Position">](valve-position.png) | Position, Travel, when it was homed, Set as % and Home valve |
| **Valve > Homing**<br>`valve-homing.png` | [<img src="valve-homing.png" width="240" alt="Valve > Homing">](valve-homing.png) | Homing speeds and distances |
| **Valve > Limit**<br>`valve-limit.png` | [<img src="valve-limit.png" width="240" alt="Valve > Limit">](valve-limit.png) | The Disable switch range and toggle |
| **Motor > Jog**<br>`motor-jog.png` | [<img src="motor-jog.png" width="240" alt="Motor > Jog">](motor-jog.png) | Close and Open, hold to run; Limit override is the second finger |
| **Motor > Settings**<br>`motor-settings.png` | [<img src="motor-settings.png" width="240" alt="Motor > Settings">](motor-settings.png) | Speeds, acceleration, steps per turn and direction |

## Pressure sensor, System and Diagnostics

| Screen | Capture | Shows |
| --- | --- | --- |
| **Pressure sensor**<br>`pressure-sensor.png` | [<img src="pressure-sensor.png" width="240" alt="Pressure sensor">](pressure-sensor.png) | The reading, the signal and the two calibration points |
| **Capture question**<br>`capture-question.png` | [<img src="capture-question.png" width="240" alt="Capture question">](capture-question.png) | Asked before a calibration point is captured |
| **System**<br>`system.png` | [<img src="system.png" width="240" alt="System">](system.png) | IP address, MAC address, clock, Network control, firmware, up time and Power |
| **IP address and mask**<br>`ip-address.png` | [<img src="ip-address.png" width="240" alt="IP address and mask">](ip-address.png) | Address, Mask, Save and Default |
| **Diagnostics**<br>`diagnostics.png` | [<img src="diagnostics.png" width="240" alt="Diagnostics">](diagnostics.png) | Raw readings of the sensor, the motor and the board |

## Faults, warnings and questions

| Screen | Capture | Shows |
| --- | --- | --- |
| **Fault F-1**<br>`fault-f1.png` | [<img src="fault-f1.png" width="240" alt="Fault F-1">](fault-f1.png) | A latched fault: FAULT in the header, the tile edged red, Reset fault on the strip |
| **Warning W-1**<br>`limit-switch-question.png` | [<img src="limit-switch-question.png" width="240" alt="Warning W-1">](limit-switch-question.png) | The limit switch question: Skip or Set as closed |
| **Warning W-2**<br>`warning-w2.png` | [<img src="warning-w2.png" width="240" alt="Warning W-2">](warning-w2.png) | No pressure response: AUTO has ended, OK on the strip |
| **Unsaved changes**<br>`unsaved-changes.png` | [<img src="unsaved-changes.png" width="240" alt="Unsaved changes">](unsaved-changes.png) | The yellow save icon asks to save the settings in use |
| **Factory reset**<br>`factory-reset.png` | [<img src="factory-reset.png" width="240" alt="Factory reset">](factory-reset.png) | Asked before the motor, valve and PID settings are reset |

## Taking the captures again

```
node simulator/tests/run.js --png <folder>
```

This saves a capture at each step of the simulator tests. Copy the ones that changed into this folder.
