# iPC Frappy driver

A [Frappy](https://github.com/SampleEnvironment/frappy) driver that lets a SECoP server read and drive the iPC pressure controller. It talks to the controller's text port, TCP 8888, and runs on a Frappy server against the board.

`docs/text-port-commands.md` is the specification of the commands, replies and error codes the driver uses.

## Contents

- [What the SECoP client sees](#what-the-secop-client-sees)
- [Files](#files)
- [Set-up](#set-up)
- [How it behaves](#how-it-behaves)
- [Tests](#tests)

## What the SECoP client sees

The modules follow the SECoP controller pattern: `pressure` controls `valve`. Writing either target takes control, as SECoP clients expect.

| Module | Parameter or command | What it is |
| --- | --- | --- |
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

`alarm`, `tolerance` and `ok_warning` are not SECoP predefined names, so the node lists them as `_alarm`, `_tolerance` and `_ok_warning`.

The link module (`ipc_io`) is not exported, so no SECoP client can send raw text commands. Homing, calibration, settings, tuning and the motor are on the touchscreen only; the driver cannot reach them.

## Files

| File | Holds |
| --- | --- |
| `frappy_ipc/ipc.py` | The driver: `IpcIO`, `Pressure`, `Valve` |
| `cfg/ipc_cfg.py` | A Frappy configuration for one board |
| `tests/fake_ipc.py` | A stand-in board, for testing without hardware |
| `tests/run.py` | Runs the driver against the stand-in board or the simulated board |

## Set-up

1. Install Frappy with `pip install frappy-core` (tested with 0.20.4), or use the Frappy the server already runs.
2. Put the `secop` folder on `PYTHONPATH`, or copy `frappy_ipc` next to the server's other driver packages.
3. Copy `cfg/ipc_cfg.py` into the server's configuration folder. Make one copy per board, each with its own node name and SECoP port.
4. Set the board's address in the `IPC_URI` environment variable, or edit the default in the file. The form is `tcp://<address shown on the screen>:8888`.
5. Start the server:

   ```
   frappy-server -c ipc_cfg.py ipc
   ```

## How it behaves

**One connection.** The board takes one connection on port 8888 at a time, and a new one replaces the old. While the Frappy server is connected, close any terminal on that port.

**Polling.** Each poll sends its queries on one line.

| Poll | Every | Line sent |
| --- | --- | --- |
| Pressure | 1 s | `PRESSURE?;SETPOINT?;MODE?;ALARM?` |
| Valve | 2 s | `VALVE?;MODE?;VALVE MOVING?` |

The driver also sends `SETPOINT`, `MODE AUTO`, `MODE MANUAL`, `VALVE SET`, `STOP`, `ALARM RESET`, `ALARM OK` and `BAROMETER?`.

**Errors.** An `ERR` from the board comes back to the SECoP client as an error with the board's text, for example `Switch to MANUAL first`.

**Network control.** The board takes set commands only while Menu > System > Network control is On. It is On on a new board and saved as it is set. While it is Off the readings still update, and every change (target, mode, valve, stop, alarm reset) comes back as `Read only over the network` (`ERR 211`).

**OFF and Shut down.** The board starts OFF at every power-up and is turned on only at its screen.

| Board state | `MODE?` answers | Both modules read | A new target |
| --- | --- | --- | --- |
| OFF | `OFF` | DISABLED, `controller off: turn it on at the screen` | Refused, `ERR 213` `Controller is off` |
| Shut down closing the valve | `SHUTDOWN` | BUSY; `stop` cancels it | Refused, `ERR 213` `Shutting down, STOP to cancel` |

**Alarms.** The board raises three faults (F-1 to F-3) and two warnings (W-1, W-2). `alarm` lists the tags of all active ones, faults first, as `ALARM?` gives them.

## Tests

```
pip install frappy-core
python secop/tests/run.py
python secop/tests/run.py --sim
```

The test starts a board and a Frappy server on local ports and checks, in order: readings, the setpoint starting AUTO, a valve move ending AUTO, stop, reset, an out-of-range setpoint, the room pressure, a fault, Network control Off (`ERR 211`), shutting down and OFF (`ERR 213`).

| Run | The board is | Needs | Time |
| --- | --- | --- | --- |
| `run.py` | `tests/fake_ipc.py`. It reaches the screen-only states through `TEST` lines that a real board does not have | Python, frappy-core | 15 s |
| `run.py --sim` | The firmware compiled to WebAssembly (`simulator/tests/tcp-board.js`), run ten times faster than the clock and started three times: Network control On, Off, and shut down | Python, frappy-core, node | 15 s |
