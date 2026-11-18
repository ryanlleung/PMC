# iPC Modbus TCP register map

Map version 4, firmware 0.18.0. Register 0 holds the map version.

- TCP port 502, up to 2 clients at once. A third connection replaces the client idle longest. A client that stops in the middle of a request is dropped after 5 s.
- Function codes: 1 (read coils), 3 (read holding registers), 5 (write single coil), 16 (write multiple registers). Every other code answers exception 1.
- The unit id is not checked and is echoed back. Any value works (1 or 255 are usual).
- Addresses are 0-based protocol addresses. A client that numbers from 1, or uses 40001-style numbers, adds 1 (holding register 10 is 40011).
- Floats are IEEE 754 single precision in two registers, high word first (big-endian, ABCD). NaN means there is no value: no pressure reading, no target set, or the valve position is not set.
- The values are updated once a second (the pressure read), so polling faster than 1 Hz adds nothing.

## Writes

Writes are taken only while Menu > System > Network control is On on the screen (On on a new board, saved as it is set). While it is Off every write answers exception 1 and the reads go on as before. That switch is the only gate. A write to an address outside the map answers exception 2 either way.

The writable registers and command coils go through the same calls as the screen buttons, so the same checks and refusals apply. A write the controller refuses answers exception 4 and logs a NET event with the reason ("Switch to MANUAL first", "No target set", ...). While OFF or shutting down (mode 5 or 4), the AUTO coil and the valve going to register are refused ("Controller is off", "Shutting down"); STOP cancels a shut down; the target can still be written. Each accepted write is logged as a NET event with the client's IP address ("Modbus 192.168.1.10: setpoint 7.5 mbar") and shown on the screen's status strip for 5 s as "Network: ...". Reads and refusals show nothing on the screen.

Rewriting the target with the value it already has changes nothing (no event; the Stable timer runs on), and a refused write is logged once a minute at most. A STOP on the screen does not hold the network's moves.

| Exception | Meaning |
| --- | --- |
| 1 | Function code not supported, or writes are off |
| 2 | Address outside the map, a read-only register or coil, or half a float |
| 3 | Bad count, byte count or coil value, or a float out of range or NaN |
| 4 | Refused by the controller (same reason as on the screen, in the event log) |

## Holding registers (FC3; FC16 where marked W)

| Address | Type | Name | Units / values |
| --- | --- | --- | --- |
| 0 | U16 | Map version | 4 |
| 1 | U16 | Heartbeat | Seconds since start-up, wraps at 65535. A value that stops changing means the controller has stopped |
| 2 | U16 | Mode | 0 NOT HOMED, 1 HOMING, 2 MANUAL, 3 AUTO, 4 SHUTTING DOWN (going to 0 % for Shut down), 5 OFF (every start; turned on only at the screen) |
| 3 | U16 | Status bits | See below |
| 4 | U16 | Regulation | In AUTO: 0 no reading, 1 Regulating, 2 In band, 3 Stable. 0 outside AUTO |
| 5 | U16 | Faults latched | Bit 0 F-1, bit 1 F-2, bit 2 F-3 |
| 6 | U16 | Warnings | Bit 0 W-1, bit 1 W-2 |
| 7 | U16 | Fault count | Latched faults |
| 8 | U16 | Event count | Events since start-up, low 16 bits. A change means a new event in the log |
| 9 | U16 | Reserved | 0 |
| 10-11 | F32 | Pressure | mbar absolute. NaN without a valid reading |
| 12-13 | F32 W | Target | mbar, kept to 0.1 mbar, 0 to the sensor range. NaN until set. Write: as Set target on the screen, in any mode |
| 14-15 | F32 | Valve position | % open. NaN when the position is not set |
| 16-17 | F32 W | Valve going to | % open, where the valve is going. Write: as Go to % on Home, 0 to 100, MANUAL only |
| 18-19 | F32 | Sensor range | mbar, full scale of the sensor, 1034.2 (15 psia) |
| 20-21 | F32 | Excitation | V, the Druck supply. NaN if the Power Monitor does not answer |
| 22-23 | F32 | Controller temperature | degC, Power Monitor die. NaN if it does not answer |
| 24-25 | U32 | Uptime | Seconds since start-up, high word first. Carries on past 49.7 days |
| 26 | U16 | PID zone in use | 1 to 4. In AUTO the zone the PID table has switched to; otherwise the zone AUTO would start with now |
| 27 | U16 W | Reserved | Always 2. A write of 2 is taken and changes nothing; any other value answers exception 3 |
| 28-29 | F32 W | Boundary 1 | mbar, kept to 0.1 mbar, 0.1 to 1000: zone 1 below, zone 2 above |
| 30-31 | F32 W | Boundary 2 | mbar, as boundary 1: zone 2 below, zone 3 above |
| 32 | U16 W | Hysteresis | % of the boundary, 0 to 40. On the pressure a zone changes only this far past a boundary |
| 33-38 | F32 W | Zone 1 KP, TI, KD | KP % open per mbar (0.01 to 100), TI min (0 to 100, 0 off), KD % per mbar/s (0 to 100), each kept to 0.01 |
| 39-44 | F32 W | Zone 2 KP, TI, KD | As zone 1 |
| 45-50 | F32 W | Zone 3 KP, TI, KD | As zone 1 |
| 51-56 | F32 W | Zone 4 KP, TI, KD | As zone 1 |
| 57-58 | F32 W | Boundary 3 | mbar, 0 or 0.1 to 1000, kept to 0.1 mbar: zone 3 below, zone 4 above; 0 = three zones (zone 3 above boundary 2, zone 4 unused) |

Reading past address 58 answers exception 2. Read 0 to 58 (59 registers) in one request.

### PID table (registers 26 to 58)

The same settings as Menu > PID on the screen (Gains, Zones and Settings tabs) and the text port's `PIDS` and `TABLE` commands. A write is a tuning save, as Save on the screen (it also saves any other settings in use and not yet saved): it is taken in AUTO too, from the next control step, and kept through a power cycle. One FC16 can write any run of registers 27 to 58 (each float whole); the values are checked first, then saved together in one flash write. A write that changes nothing writes nothing. A value out of range or NaN answers exception 3 and changes nothing. Boundary 2 must be above boundary 1 by more than the hysteresis either side (B1 x (1 + H) below B2 x (1 - H)), and boundary 3, unless 0, above boundary 2 the same way; a write that breaks this answers exception 4 and logs the reason. To move the boundaries past each other, write 27 to 58 in one request (the gains as read), or set boundary 3 to 0 first. Register 26 is read only.

The zone in use changes in AUTO when the lower of the pressure and the setpoint passes a boundary by the hysteresis, so going down the setpoint's zone takes over at once. Each change is logged ("AUTO PID zone 3 to 2, pressure 16.9 mbar"). A change re-bases the integral, so the valve does not jump.

### Status bits (register 3, and status coils 16-25)

| Bit | Coil | Meaning when 1 |
| --- | --- | --- |
| 0 | 16 | Pressure reading valid |
| 1 | 17 | Valve position set (homed, set with Set as %, or taken from the saved position) |
| 2 | 18 | Homed on the closed switch, also when kept through Shut down and a power-off |
| 3 | 19 | Valve moving |
| 4 | 20 | Closed switch pressed |
| 5 | 21 | A fault is latched |
| 6 | 22 | A warning is showing (W-1 or W-2) |
| 7 | 23 | Target set |
| 8 | 24 | Network writes on (Menu > System > Network control On) |
| 9 | 25 | Reserved: always 1. The firmware does not measure the motor supply, so nothing on Modbus shows loss of motor power |

### Faults and warnings (registers 5 and 6)

| Bit | Fault | Warning |
| --- | --- | --- |
| 0 | F-1 No pressure signal (also the excitation under 7 V or tripped) | W-1 Limit switch pressed (Skip or Set as closed waiting on the screen) |
| 1 | F-2 Limit switch not found while homing | W-2 No pressure response (AUTO ended) |
| 2 | F-3 Limit switch stays pressed (stuck, or pressed during an opening move) | Always 0 |
| 3 to 5 | Always 0 | Always 0 |

W-2 shows on the strip until OK. W-1 clears when the question is answered on the screen.

## Coils (FC1; FC5 where marked W)

| Address | Name | Read | Write 1 (FF00) | Write 0 (0000) |
| --- | --- | --- | --- | --- |
| 0 | AUTO (W) | 1 in AUTO | Start AUTO (entry checks as on the screen) | Switch to MANUAL (the valve ramps down and holds) |
| 1 | STOP (W) | 0 | STOP (in AUTO: to MANUAL, nothing latched; also ends homing, a shut down or a sensor capture) | Nothing |
| 2 | RESET (W) | 1 while a fault is latched | Clear the latches whose cause has gone | Nothing |
| 3 | Warning OK (W) | 1 while W-2 shows | OK the warning, as on the strip | Nothing |
| 16-25 | Status | Status bits 0-9 above | Exception 2 | Exception 2 |

Coils 4 to 15 are not in the map (exception 2). Homing, the switch question, calibration and settings are not on Modbus.

## Example: decoding the pressure

Registers 10-11 read `0x4094 0x0000`: the 32-bit word is `0x40940000`, which is 4.625 mbar.
