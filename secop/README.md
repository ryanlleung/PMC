# mm4 Frappy driver

A [Frappy](https://github.com/SampleEnvironment/frappy) driver so a SECoP
server can read and drive the mm4. It talks to the board's text port (TCP
8888).

This is a reference implementation of the suggested SECoP mapping in the
mm4 Remote Commands handover, for whoever writes the production driver. The
handover is the specification of the commands, replies and error codes.

## What the SECoP client sees

The modules follow the SECoP controller pattern: `pressure` controls
`valve`. Writing either target takes control, as SECoP clients expect.

| Module | Parameter or command | What it is |
|---|---|---|
| `pressure` | `value` | Pot pressure, mbar |
| | `target` | Setpoint, mbar, 0.1 mbar steps. Writing it sets the setpoint and starts AUTO |
| | `control_active` | True in AUTO |
| | `status` | IDLE in MANUAL, or in AUTO within `tolerance` of the setpoint; BUSY while AUTO is still getting there, or while homing; WARN for a warning or a valve that is not homed; ERROR for a fault or no reading |
| | `alarm` | Latched fault `F-n` or warning `W-n`, empty if none |
| | `tolerance` | Band for IDLE in AUTO, default 0.5 mbar (driver only, not the board's target band) |
| | `stop` | Stop the valve now; in AUTO this ends AUTO |
| | `reset` | Clear latched faults whose cause has gone |
| | `ok_warning` | Acknowledge the warning, as OK on the screen |
| `valve` | `value` | Valve position, % open |
| | `target` | Valve position to go to. Writing it ends AUTO first (MANUAL) |
| | `controlled_by` | `pressure` in AUTO, `self` in MANUAL |
| | `stop` | As above |
| `room_pressure` | `value` | Room pressure from the Pressure 21 Click, mbar (leave the module out if not fitted) |

`alarm`, `tolerance` and `ok_warning` are not SECoP predefined names, so
the node lists them as `_alarm`, `_tolerance` and `_ok_warning`. The link
module (`mm4_io`) is not exported, so no SECoP client can send raw text
commands.

Homing, calibration, settings, tuning and the motor stay on the touch
screen. The driver has no way to reach them.

## Files

- `frappy_mm4/mm4.py`: the driver (`Mm4IO`, `Pressure`, `Valve`).
- `cfg/mm4_cfg.py`: a Frappy configuration for one board.
- `tests/fake_mm4.py`: a stand-in board for testing without hardware.
- `tests/run.py`: runs the driver against the stand-in board.

## Setting it up

1. `pip install frappy-core` (tested with 0.20.4), or use the Frappy the
   server already runs.
2. Put the `secop` folder on `PYTHONPATH`, or copy `frappy_mm4` next to the
   server's other driver packages.
3. Copy `cfg/mm4_cfg.py` into the server's configuration folder. Set the
   board's address in `MM4_URI` or edit the default in the file
   (`tcp://<address shown on the screen>:8888`). Copy it once per board,
   with its own node name and SECoP port.
4. `frappy-server -c mm4_cfg.py mm4`

The driver needs the firmware with the remote command set (`SETPOINT`,
`MODE AUTO`, `MODE MANUAL`, `VALVE SET`, `STOP`, `ALARM RESET`, `ALARM OK`,
`PRESSURE?`, `SETPOINT?`, `MODE?`, `VALVE?`, `VALVE MOVING?`, `BAROMETER?`, `ALARM?`). That firmware and this driver are released together;
firmware 0.12.21 and earlier does not answer these commands.

## Things to know

- The board takes one connection on port 8888 at a time, and a new one
  replaces the old. While the Frappy server is connected, close any terminal
  and the logger (`tools/logger`).
- Each reading is its own query, and each poll chains its queries on one
  line: the pressure poll (every second) sends `PRESSURE?;SETPOINT?;MODE?;ALARM?`, the
  valve poll (every 2 s) `VALVE?;MODE?;VALVE MOVING?`.
- An `ERR` from the board comes back to the SECoP client as an error with
  the board's text, for example `Switch to MANUAL first`.
- From firmware 0.13.4 the board takes set commands over the network only
  while Menu > System > Remote control is On (On on a new
  board from 0.13.7, saved as it is set). While Off, the readings still update and
  every change (target, mode, valve, stop, alarm reset) comes back as
  `Read only over the network` (ERR 211).
- The driver has been tested against `tests/fake_mm4.py` only, not yet
  against a board.

## Testing

    pip install frappy-core
    python secop/tests/run.py

It starts the stand-in board and a Frappy server on local ports, then checks
readings, the setpoint starting AUTO, a valve move ending AUTO, stop, reset,
an out-of-range setpoint and the room pressure.
