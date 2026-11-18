# iPC Frappy driver

A [Frappy](https://github.com/SampleEnvironment/frappy) driver so a SECoP
server can read and drive the iPC pressure controller. It talks to the board's text port (TCP
8888).

It runs on a Frappy server against the board. `docs/text-port-commands.md`
is the specification of the commands, replies and error codes it uses.

## What the SECoP client sees

The modules follow the SECoP controller pattern: `pressure` controls
`valve`. Writing either target takes control, as SECoP clients expect.

| Module | Parameter or command | What it is |
|---|---|---|
| `pressure` | `value` | Pot pressure, mbar |
| | `target` | Setpoint, mbar, 0.1 mbar steps. Writing it sets the setpoint and starts AUTO |
| | `control_active` | True in AUTO |
| | `status` | IDLE in MANUAL, or in AUTO within `tolerance` of the setpoint; BUSY while AUTO is still getting there, while homing, or while shutting down; WARN for a warning or a valve that is not homed; DISABLED while the controller is off; ERROR for a fault or no reading |
| | `alarm` | Latched fault `F-n` or warning `W-n`, empty if none |
| | `tolerance` | Band for IDLE in AUTO, default 0.5 mbar (driver only, not the board's target band) |
| | `stop` | Stop the valve now; in AUTO this ends AUTO |
| | `reset` | Clear latched faults whose cause has gone |
| | `ok_warning` | Acknowledge the warning, as OK on the screen |
| `valve` | `value` | Valve position, % open |
| | `target` | Valve position to go to. Writing it ends AUTO first (MANUAL) |
| | `controlled_by` | `pressure` in AUTO, `self` in MANUAL |
| | `status` | IDLE, or BUSY while moving, homing or shutting down; WARN while not homed; DISABLED while the controller is off |
| | `stop` | As above |
| `room_pressure` | `value` | Room pressure from the Pressure 21 Click, mbar (leave the module out if not fitted) |

`alarm`, `tolerance` and `ok_warning` are not SECoP predefined names, so
the node lists them as `_alarm`, `_tolerance` and `_ok_warning`. The link
module (`ipc_io`) is not exported, so no SECoP client can send raw text
commands.

Homing, calibration, settings, tuning and the motor stay on the touch
screen. The driver has no way to reach them.

## Files

- `frappy_ipc/ipc.py`: the driver (`IpcIO`, `Pressure`, `Valve`).
- `cfg/ipc_cfg.py`: a Frappy configuration for one board.
- `tests/fake_ipc.py`: a stand-in board for testing without hardware.
- `tests/run.py`: runs the driver against the stand-in board, or against
  the simulated board (the firmware itself, `simulator/tests/tcp-board.js`).

## Setting it up

1. `pip install frappy-core` (tested with 0.20.4), or use the Frappy the
   server already runs.
2. Put the `secop` folder on `PYTHONPATH`, or copy `frappy_ipc` next to the
   server's other driver packages.
3. Copy `cfg/ipc_cfg.py` into the server's configuration folder. Set the
   board's address in `IPC_URI` or edit the default in the file
   (`tcp://<address shown on the screen>:8888`). Copy it once per board,
   with its own node name and SECoP port.
4. `frappy-server -c ipc_cfg.py ipc`

The driver uses these commands (`SETPOINT`, `MODE AUTO`,
`MODE MANUAL`, `VALVE SET`, `STOP`, `ALARM RESET`, `ALARM OK`, `PRESSURE?`,
`SETPOINT?`, `MODE?`, `VALVE?`, `VALVE MOVING?`, `BAROMETER?`, `ALARM?`). It is checked
against the command set of firmware 0.18.0, on the simulated board.

## Things to know

- The board takes one connection on port 8888 at a time, and a new one
  replaces the old. While the Frappy server is connected, close any terminal
  on that port.
- Each reading is its own query, and each poll chains its queries on one
  line: the pressure poll (every second) sends `PRESSURE?;SETPOINT?;MODE?;ALARM?`, the
  valve poll (every 2 s) `VALVE?;MODE?;VALVE MOVING?`.
- An `ERR` from the board comes back to the SECoP client as an error with
  the board's text, for example `Switch to MANUAL first`.
- The board takes set commands over the network only while Menu > System >
  Network control is On (On on a new board, saved as it is set). While Off,
  the readings still update and every change (target, mode, valve, stop,
  alarm reset) comes back as `Read only over the network` (ERR 211).
- The board starts OFF at every power-up and is turned on only at its
  screen. While OFF (`MODE?` answers `OFF`) both modules read DISABLED,
  `controller off: turn it on at the screen`. While Shut down closes the
  valve (`SHUTDOWN`) both read BUSY, and `stop` cancels it. In both the
  board refuses a new target with ERR 213 (`Controller is off`, or
  `Shutting down, STOP to cancel`).
- The board raises three faults (F-1 to F-3) and two warnings (W-1, W-2);
  `alarm` lists the tags of all active ones, faults first, as `ALARM?` gives them.
- The driver's tests run it against `tests/fake_ipc.py` and the simulated
  board.

## Testing

    pip install frappy-core
    python secop/tests/run.py
    python secop/tests/run.py --sim

It starts a board and a Frappy server on local ports, then checks readings,
the setpoint starting AUTO, a valve move ending AUTO, stop, reset, an
out-of-range setpoint and the room pressure, then a fault, Network control
Off (ERR 211), shutting down and OFF (ERR 213).

Without `--sim` the board is `tests/fake_ipc.py`, which reaches the
screen-only states through `TEST` lines a real board does not have. With
`--sim` it is the firmware compiled to WebAssembly (needs node), run ten
times faster than the clock, and started three times: Network control On,
Off (`--remote-off`) and shut down (`--off`). It takes about two minutes.
