# iPC text port commands

The text port and Modbus take the same operator command set, and nothing else: what an operator can use and judge away from the cryostat. The text port and Modbus also take the PID table: its four zones' gains and its boundaries (`PIDS?`, `PIDS SET`, `TABLE?`, `TABLE SET`; Modbus registers 26 to 58). Homing, calibration, the other settings and the motor are on the touch screen. `firmware/remote.c` is the text port; the Modbus registers and coils for the same commands are in `docs/modbus-register-map.md`.

Connect to TCP port 8888 (for example PuTTY in Raw mode). One client at a time; a new connection replaces the old. Send upper case ASCII, a line ending in LF or CR LF, at most 96 characters. Replies end in CR LF.

- No echo, and nothing is sent unasked: faults, warnings and switch changes go to the event log (`EVENT?`) and show in `ALARM?` and `MODE?`.
- A query (ending in `?`) replies one line starting with its own words, the value then the unit: `PRESSURE 4.625 mbar`, `VALVE 34.12 %`, `VALVE NONE` when there is no value. Pressure 3 dp, setpoint 1 dp, valve 2 dp, room pressure 2 dp.
- A set command replies nothing when accepted, and `ERR <code> <text>` when refused. End a line that sets something with a query (`SETPOINT 5;MODE AUTO;MODE?`) to always get one line back.
- Commands can share a line with `;`: they run in order, the query replies come back on one line joined by `;`, and the first `ERR` ends the line. `EVENT?` is the only multi-line reply and must be on a line of its own.

| Command | Reply or action |
| --- | --- |
| `SETPOINT <mbar>` | The pressure setpoint, 0 to the sensor range, kept to 0.1 mbar. Does not start AUTO |
| `MODE AUTO` / `MODE MANUAL` | Start regulating after the entry checks / stop regulating, the valve stays where it is. Both refused (`ERR 213`) while OFF or shutting down: only the screen turns it on |
| `VALVE SET <%>` | Move the valve, 0 to 100 % in 0.1 steps; MANUAL only, valve homed |
| `STOP` | Stop the valve now; in AUTO it also ends AUTO. Cancels homing, a shut down and a calibration capture. The only stop |
| `ALARM RESET` / `ALARM OK` | Clear the latched faults whose cause has gone / acknowledge the warning, as the screen's OK |
| `TIME SET <YYYY-MM-DD> <HH:MM:SS>` | Set the clock |
| `PIDS SET <zone>,<kp>,<ti>[,<kd>]` | Set and save the gains of PID zone 1 to 4, as Save on the PID page: KP in %/mbar, TI in min (0 off), KD in %/(mbar/s) (0 off, left as it is when not given), 2 dp. KP 0.01 to 100, TI and KD 0 to 100. Taken in AUTO too, at the next control step. Each change is a settings save in flash (it also saves any other setting in use on the screen and not yet saved), so send it when the gains change, not every poll; gains equal to the present ones write nothing |
| `TABLE SET <b1>,<b2>,<b3>,<h>` | The PID table's boundaries and hysteresis, saved as `PIDS SET` (as Save on PID > Zones): boundaries B1, B2 and B3 in mbar (1 dp, 0.1 to 1000; zone 1 below B1, zone 2 to B2, zone 3 to B3, zone 4 above; B3 0 for three zones), hysteresis H in % of the boundary (0 to 40). B2 must be above B1, and B3 (unless 0) above B2, by more than H either side (`ERR 102`) |
| `PRESSURE?` | `PRESSURE 4.625 mbar` or `PRESSURE NONE` |
| `SETPOINT?` | `SETPOINT 5.0 mbar` or `SETPOINT NONE` |
| `MODE?` | `MODE MANUAL`, `MODE AUTO`, `MODE HOMING`, `MODE NOT HOMED`, `MODE SHUTDOWN` (going to 0 % for Shut down) or `MODE OFF` |
| `VALVE?` / `VALVE MOVING?` | `VALVE 34.12 %` or `VALVE NONE` / `VALVE MOVING 1` while moving or homing, else 0 |
| `BAROMETER?` | `BAROMETER 1013.25 mbar` or `BAROMETER NONE` (Pressure 21 Click not fitted or not reading) |
| `ALARM?` | `ALARM NONE`, or the active alarms joined by `\|`, faults first: `<tag>,<0\|1>,<text>`. For a fault, 1 when `ALARM RESET` clears it now; for a warning, 1 when `ALARM OK` clears it |
| `EVENT? [<n>]` | `EVENT <n>`, then n lines `<age s>,<tag>,<text>`, newest first; default 1, at most 100 |
| `TIME?` | `TIME 2026-10-27 11:05:00` or `TIME NONE` |
| `PIDS?` | `PIDS 2,0.06,0.60,0.00,0.12,0.82,0.00,0.80,0.94,0.00,0.40,0.25,0.00` (the factory gains): the zone in use (in AUTO the one the table has switched to, else the one AUTO would start with), then KP, TI, KD of zones 1 to 4 |
| `TABLE?` | `TABLE 6.0,10.0,14.0,15` (the factory table): the PID table's B1, B2, B3 in mbar and H in % |
| `*IDN?` | `MercShore,iPC,<chip id>,<version>` |

Error codes: 100 unknown command (anything not in the table above when network writes are enabled; with writes off, unrecognised input also gets 211), 101 expected form, 102 outside range, 103 line too long, 104 `EVENT?` not on its own line, 201 homing, STOP to cancel, 202 switch to MANUAL first, 203 reset the fault first, 204 home the valve first, 205 no valid pressure, 206 set a setpoint first, 207 setpoint outside the sensor range, 208 not used, 209 control settings not set, 210 not reset, cause still present, 211 read only over the network, 212 the valve cannot move now (the limit switch, Close or Open held on the screen, the switch question waiting, a calibration point being taken) or the clock is not running, 213 controller off or shutting down (turned on at the screen).
