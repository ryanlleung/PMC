# Modbus TCP register map

Map version 3, from firmware 0.16.14: version 2 (registers 0 to 50, firmware 0.15.15 to 0.16.13) with PID set 4 and boundary 3 added as registers 51 to 58. Version 2 was version 1 (registers 0 to 25, checked against 0.14.8) with the gain schedule added as registers 26 to 50. Nothing in versions 1 or 2 moved. The map was introduced in 0.12.0; writes have been on from 0.12.6. Ethernet build only (`PMC_LINK_ETHERNET 1`).

- TCP port 502, up to 2 clients at once. A third connection replaces the client idle longest. A client that stops in the middle of a request is dropped after 5 s.
- Function codes: 1 (read coils), 3 (read holding registers), 5 (write single coil), 16 (write multiple registers). Every other code answers exception 1.
- The unit id is not checked and is echoed back. Any value works (1 or 255 are usual).
- Addresses are 0-based protocol addresses. A client that numbers from 1, or uses 40001-style numbers, adds 1 (holding register 10 is 40011).
- Floats are IEEE 754 single precision in two registers, high word first (big-endian, ABCD). NaN means there is no value: no pressure reading, no target set, or the valve position is not set.
- The values are updated once a second (the pressure read), so polling faster than 1 Hz adds nothing.

## Writes

Writes are built in from 0.12.6, so anyone who can reach port 502 can set the target and run the valve. From 0.13.4 writes need Menu > System > Remote control On on the screen (On by default from 0.13.7, Off to 0.13.6, kept by Save settings); while it is Off every write answers exception 1 and the reads go on as before. The build flag `PMC_MODBUS_WRITE` went in 0.16.28: the switch is the only gate. A write to an address outside the map answers exception 2 either way.

With writes on, the writable registers and command coils go through the same calls as the screen buttons, so the same checks and refusals apply. A write the controller refuses answers exception 4 and logs a NET event with the reason ("Switch to MANUAL first", "No target set", ...). While OFF or shutting down (mode 5 or 4), the AUTO coil and the valve going to register are refused ("Controller is off", "Shutting down"); STOP cancels a shut down; the target can still be written. Each accepted write is logged as a NET event with the client's IP address.

Rewriting the target with the value it already has changes nothing (no event; the W-2 and Stable timers run on), and a refused write is logged once a minute at most. A STOP on the screen does not hold the network's moves (the hold of 0.13.31 was removed in 0.14.16).

| Exception | Meaning |
| --- | --- |
| 1 | Function code not supported, or writes are off |
| 2 | Address outside the map, a read-only register or coil, or half a float |
| 3 | Bad count, byte count or coil value, or a float out of range or NaN |
| 4 | Refused by the controller (same reason as on the screen, in the event log) |

## Holding registers (FC3; FC16 where marked W)

| Address | Type | Name | Units / values |
| --- | --- | --- | --- |
| 0 | U16 | Map version | 2 (1 before 0.15.15) |
| 1 | U16 | Heartbeat | Seconds since start-up, wraps at 65535. A value that stops changing means the controller has stopped |
| 2 | U16 | Mode | 0 NOT HOMED, 1 HOMING, 2 MANUAL, 3 AUTO, 4 SHUTTING DOWN (going to 0 % for Shut down), 5 OFF (from 0.15.0; every start from 0.15.1; turned on only at the screen) |
| 3 | U16 | Status bits | See below |
| 4 | U16 | Regulation | In AUTO: 0 no reading, 1 Regulating, 2 In band, 3 Stable. 0 outside AUTO |
| 5 | U16 | Faults latched | Bit 0 F-1 ... bit 5 F-6 |
| 6 | U16 | Warnings | Bit 0 W-1 ... bit 5 W-6 |
| 7 | U16 | Fault count | Latched faults |
| 8 | U16 | Event count | Events since start-up, low 16 bits. A change means a new event in the log |
| 9 | U16 | Reserved | 0 |
| 10-11 | F32 | Pressure | mbar absolute. NaN without a valid reading |
| 12-13 | F32 W | Target | mbar, kept to 0.1 mbar, 0 to the sensor range. NaN until set. Write: as Set target on the screen, in any mode |
| 14-15 | F32 | Valve position | % open. NaN when the position is not set |
| 16-17 | F32 W | Valve going to | % open, where the valve is going. Write: as Go to % on Home, 0 to 100, MANUAL only |
| 18-19 | F32 | Sensor range | mbar, full scale of the sensor, 1034.2 (15 psia; one sensor per board from 0.16.2) |
| 20-21 | F32 | Excitation | V, the Druck supply. NaN if the Power Monitor does not answer |
| 22-23 | F32 | Controller temperature | degC, Power Monitor die. NaN if it does not answer |
| 24-25 | U32 | Uptime | Seconds since start-up, high word first |
| 26 | U16 | PID set in use | 1 to 4 (1 to 3 to 0.16.13). In AUTO the set the gain schedule has switched to; otherwise the set AUTO would start with now. 1 with the schedule Off (0.15.15 to 0.16.19) |
| 27 | U16 W | PID table (gain schedule) | Always 2 (on the pressure). No setting is behind it from 0.16.30: a write of 2 is taken and changes nothing, any other value answers exception 3 (0, Off, to 0.16.19; 1, the setpoint, 0.15.15 to 0.15.19) |
| 28-29 | F32 W | Boundary 1 | mbar, kept to 0.1 mbar, 0.1 to 1000: set 1 below, set 2 above |
| 30-31 | F32 W | Boundary 2 | mbar, as boundary 1: set 2 below, set 3 above |
| 32 | U16 W | Hysteresis | % of the boundary, 0 to 40. On the pressure a set changes only this far past a boundary |
| 33-38 | F32 W | Set 1 KP, TI, KD | KP % open per mbar (0.01 to 100), TI min (0 to 100, 0 off), KD % per mbar/s (0 to 100), each kept to 0.01 |
| 39-44 | F32 W | Set 2 KP, TI, KD | As set 1 |
| 45-50 | F32 W | Set 3 KP, TI, KD | As set 1 |
| 51-56 | F32 W | Set 4 KP, TI, KD | As set 1 (version 3, 0.16.14) |
| 57-58 | F32 W | Boundary 3 | mbar, 0 or 0.1 to 1000, kept to 0.1 mbar: set 3 below, set 4 above; 0 = three sets (set 3 above boundary 2, set 4 unused) (version 3, 0.16.14) |

Reading past address 58 answers exception 2. Read 0 to 58 (59 registers) in one request, 0 to 50 for the version 2 map or 0 to 25 for the version 1 map alone. Register 0 says which.

### Gain schedule (registers 26 to 58)

The same settings as Menu > PID tuning on the screen (PID and PID table tabs) and the text port's `PIDS` and `SCHED` commands. A write is a tuning save, as the screen's Save settings: it is taken in AUTO too, from the next control step, and kept through a power cycle. One FC16 can write any run of registers 27 to 58 (each float whole); the values are checked first, then saved together in one flash write. A write that changes nothing writes nothing. A value out of range or NaN answers exception 3 and changes nothing. Boundary 2 must be above boundary 1 by more than the hysteresis either side (B1 x (1 + H) below B2 x (1 - H)), and boundary 3, unless 0, above boundary 2 the same way; a write that breaks this answers exception 4 and logs the reason. To move the boundaries past each other, write 27 to 58 in one request (the gains as read), or set boundary 3 to 0 first. Register 26 is read only.

The set in use changes in AUTO when the lower of the pressure and the setpoint (schedule 2; the lower of the two since 0.16.9, so going down the setpoint's set takes over at once) passes a boundary by the hysteresis. Each change is logged ("AUTO PID set 3 to 2, pressure 16.9 mbar"). A change re-bases the integral, so the valve does not jump.

### Status bits (register 3, and status coils 16-25)

| Bit | Coil | Meaning when 1 |
| --- | --- | --- |
| 0 | 16 | Pressure reading valid |
| 1 | 17 | Valve position set (homed, set by hand or from the saved position) |
| 2 | 18 | Homed on the closed switch, also when kept through Shut down and a power-off (from 0.15.0) |
| 3 | 19 | Valve moving |
| 4 | 20 | Closed switch pressed |
| 5 | 21 | A fault is latched |
| 6 | 22 | A warning is showing (W-1 to W-6) |
| 7 | 23 | Target set |
| 8 | 24 | Remote writes on (Remote control On, 0.13.4) |
| 9 | 25 | Legacy motor 12 V supply flag: always 1 (sensing was removed in 0.14.3). This is not a measurement and cannot detect loss of motor power |

### Faults and warnings (registers 5 and 6)

| Bit | Fault | Warning |
| --- | --- | --- |
| 0 | F-1 No pressure signal | W-1 Switch hit early (Set as 0 % or Skip waiting) |
| 1 | F-2 Excitation | W-2 Not settling |
| 2 | F-3 Limit not found | W-3 (removed in 0.16.29, always 0) |
| 3 | F-4 Limit pressed when it should be free (stuck, or pressed during an opening move) | W-4 Switch pressed off the seat |
| 4 | F-5 (the warning W-5 from 0.16.29, always 0) | W-5 Move timed out |
| 5 | F-6 No response | W-6 (removed in 0.15.11, always 0) |

W-2, W-4 and W-5 show one at a time (the strip warning) until OK. W-1 clears when the question is answered on the screen.

## Coils (FC1; FC5 where marked W)

| Address | Name | Read | Write 1 (FF00) | Write 0 (0000) |
| --- | --- | --- | --- | --- |
| 0 | AUTO (W) | 1 in AUTO | Start AUTO (entry checks as on the screen) | Switch to MANUAL (the valve ramps down and holds) |
| 1 | STOP (W) | 0 | STOP (in AUTO: to MANUAL, nothing latched) | Nothing |
| 2 | RESET (W) | 1 while a fault is latched | Clear the latches whose cause has gone | Nothing |
| 3 | Warning OK (W) | 1 while W-2, W-4 or W-5 shows | OK the warning, as on the strip | Nothing |
| 16-25 | Status | Status bits 0-9 above | Exception 2 | Exception 2 |

Coils 4 to 15 are not in the map (exception 2). Homing, the switch question, calibration and settings are not on Modbus.

## Example: decoding the pressure

Registers 10-11 read `0x4094 0x0000`: the 32-bit word is `0x40940000`, which is 4.625 mbar.
