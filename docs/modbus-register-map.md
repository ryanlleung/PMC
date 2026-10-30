# Modbus TCP register map

Map version 1, firmware 0.12.0 (writes on from 0.12.6). Ethernet build only (`PMC_LINK_ETHERNET 1`).

- TCP port 502, up to 2 clients at once. A third connection replaces the client idle longest. A client that stops in the middle of a request is dropped after 5 s.
- Function codes: 1 (read coils), 3 (read holding registers), 5 (write single coil), 16 (write multiple registers). Every other code answers exception 1.
- The unit id is not checked and is echoed back. Any value works (1 or 255 are usual).
- Addresses are 0-based protocol addresses. A client that numbers from 1, or uses 40001-style numbers, adds 1 (holding register 10 is 40011).
- Floats are IEEE 754 single precision in two registers, high word first (big-endian, ABCD). NaN means there is no value: no pressure reading, no target set, or the valve position is not set.
- The values are updated once a second (the pressure read), so polling faster than 1 Hz adds nothing.

## Writes

`PMC_MODBUS_WRITE` in `pmc_config.h` is 1 from 0.12.6, so anyone who can reach port 502 can set the target and run the valve. From 0.13.4 writes also need Settings > System > Remote control On on the screen (On by default from 0.13.7, Off to 0.13.6, kept by Save settings); while it is Off every write answers exception 1 and the reads go on as before. With `PMC_MODBUS_WRITE 0` the server is read only and every write (FC5, FC16) answers exception 1, whatever the switch says. A write to an address outside the map answers exception 2 either way.

With writes on, the writable registers and command coils go through the same calls as the screen buttons, so the same checks and refusals apply. A write the controller refuses answers exception 4 and logs a NET event with the reason ("Switch to MANUAL first", "No target set", ...). Each accepted write is logged as a NET event with the client's IP address.

| Exception | Meaning |
| --- | --- |
| 1 | Function code not supported, or writes are off |
| 2 | Address outside the map, a read-only register or coil, or half a float |
| 3 | Bad count, byte count or coil value, or a float out of range or NaN |
| 4 | Refused by the controller (same reason as on the screen, in the event log) |

## Holding registers (FC3; FC16 where marked W)

| Address | Type | Name | Units / values |
| --- | --- | --- | --- |
| 0 | U16 | Map version | 1 |
| 1 | U16 | Heartbeat | Seconds since start-up, wraps at 65535. A value that stops changing means the controller has stopped |
| 2 | U16 | Mode | 0 NOT HOMED, 1 HOMING, 2 MANUAL, 3 AUTO |
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
| 16-17 | F32 W | Valve going to | % open, where the valve is going. Write: as Go to % on the Valve page, 0 to 100, MANUAL only |
| 18-19 | F32 | Sensor range | mbar, full scale of the sensor in use |
| 20-21 | F32 | Excitation | V, the Druck supply. NaN if the Power Monitor does not answer |
| 22-23 | F32 | Controller temperature | degC, Power Monitor die. NaN if it does not answer |
| 24-25 | U32 | Uptime | Seconds since start-up, high word first |

Reading past address 25 answers exception 2, so read 0 to 25 (26 registers) in one request.

### Status bits (register 3, and status coils 16-25)

| Bit | Coil | Meaning when 1 |
| --- | --- | --- |
| 0 | 16 | Pressure reading valid |
| 1 | 17 | Valve position set (homed, set by hand or from the saved position) |
| 2 | 18 | Homed on the closed switch |
| 3 | 19 | Valve moving |
| 4 | 20 | Closed switch pressed |
| 5 | 21 | A fault is latched |
| 6 | 22 | A warning is showing (W-1 to W-6) |
| 7 | 23 | Target set |
| 8 | 24 | Remote writes on (`PMC_MODBUS_WRITE 1` and Remote control On, 0.13.4) |
| 9 | 25 | Motor 12 V supply on: always 1 (the supply sense was removed in 0.14.3) |

### Faults and warnings (registers 5 and 6)

| Bit | Fault | Warning |
| --- | --- | --- |
| 0 | F-1 No pressure signal | W-1 Switch hit early (Set as 0 % or Skip waiting) |
| 1 | F-2 Excitation | W-2 Not settling |
| 2 | F-3 Limit not found | W-3 Controller hot |
| 3 | F-4 Limit stuck | W-4 Switch pressed off the seat |
| 4 | F-5 Motion timeout | W-5 Over range in AUTO |
| 5 | F-6 No response | W-6 Driven the wrong way |

W-2, W-4, W-5 and W-6 show one at a time (the strip warning) until OK. W-1 clears when the question is answered on the screen, W-3 when the controller cools below 50 C.

## Coils (FC1; FC5 where marked W)

| Address | Name | Read | Write 1 (FF00) | Write 0 (0000) |
| --- | --- | --- | --- | --- |
| 0 | AUTO (W) | 1 in AUTO | Start AUTO (entry checks as on the screen) | Switch to MANUAL (the valve ramps down and holds) |
| 1 | STOP (W) | 0 | STOP (in AUTO: to MANUAL, nothing latched) | Nothing |
| 2 | RESET (W) | 1 while a fault is latched | Clear the latches whose cause has gone | Nothing |
| 3 | Warning OK (W) | 1 while W-2, W-4, W-5 or W-6 shows | OK the warning, as on the strip | Nothing |
| 16-25 | Status | Status bits 0-9 above | Exception 2 | Exception 2 |

Coils 4 to 15 are not in the map (exception 2). Homing, the switch question, calibration and settings are not on Modbus.

## Example: decoding the pressure

Registers 10-11 read `0x4094 0x0000`: the 32-bit word is `0x40940000`, which is 4.625 mbar.
