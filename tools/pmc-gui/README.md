# Control test GUI

`pmc_gui.py` is a small web server on a PC that talks to the board's text port (TCP 8888) and serves one page to a browser. It holds the one connection, so every tool for the running board is on that page. The pressure, setpoint and valve, a live chart (5 min to 4 hours), the mode, the alarms and STOP are on every tab; Download CSV saves every sample, one a second.

- **Live**: Set setpoint, Start AUTO, MANUAL, Go to % and STOP; the PID gains in use (`PID?`), and Set and save (`PID SET`, firmware 0.15.8 and later); a log of every command and reply.
- **Tests**, run in this order:
  1. Valve step test: MANUAL with the valve at At %, a wait for the pressure to settle, then the valve opened by Step % and closed back, each recorded. Each response is fitted to a gain (mbar per %), a delay and a time constant, and the KP and TI the SIMC rules give for them are shown, with a Use button that puts them in the PID boxes (Set and save then applies them). Start At near where AUTO holds the setpoint you use; a larger delay closing than opening is the drive's backlash.
  2. Setpoint step test, with the gains chosen: AUTO at From, held within the band for 2 minutes, then a step to To (and back), with the time to 90 %, the overshoot, the settling time and the last minute's mean and spread.
- **Gains**: the gain schedule from `tools/gain-schedule/pmc_gains.py`, without its command line.
  - Make a gain table: at each setpoint, AUTO until the pressure holds, then MANUAL valve steps up and back, fitted to a KP and TI (`pmc_gains.py tune`). 10 to 20 minutes a setpoint. At the end it puts back the setpoint and mode it found, with the valve where the last setpoint held.
  - Download the table as CSV, or load one (from here or from `pmc_gains.py tune`).
  - The KP and TI the schedule uses at 1 to 25 mbar, blended log-log between rows.
  - Start schedule: each setpoint change sets KP and TI from the table (`pmc_gains.py run`), also during setpoint step tests, so a step test shows the scheduled gains. Paused while a table is made.
- **Board**: Reset faults and OK warning, the event log (`EVNT?`), the board clock set to the PC's time (`TIME SET`), and a command line for any text-port command.

The step fit, the SIMC rules, the table blend and the tune sequence are the ones in `pmc_gains.py`, imported from there, so the page and the command line give the same numbers.

Not on the page: the firmware's own checks (`simulator/tests`, `tools/*-test`, `tools/xbuild`, `tools/ui-preview`). They build the firmware with a compiler or the simulator with Node and never talk to a board.

## Run it with Python

Python 3.8 or later, standard library only. Keep `tools/pmc-gui` and `tools/gain-schedule` side by side (it imports the step fit from there), so nothing needs pip or admin rights. On Windows without admin, the "Windows embeddable package" zip from python.org runs after unzipping.

```
python pmc_gui.py 192.168.1.100
```

Then open http://localhost:8080. `--port 8090` picks another page port.

## Run it with Docker

```
docker build -t pmc-gui -f tools/pmc-gui/Dockerfile tools
docker run --rm -p 8080:8080 pmc-gui 192.168.1.100
```

Then open http://localhost:8080. The container reaches the board through the PC's network.

## Before you start

- Settings > System setup > Remote control On, or every button answers ERR 211.
- The board takes one text-port connection at a time, and a new one replaces the old. Close PuTTY, the logger, `pmc_gains.py` and `sp_step.py` first.
- STOP on the page and on the screen both end AUTO. If the board leaves AUTO during a step test (a fault, W-2, W-4, W-5, W-6 or STOP), the test ends and says why.
- Every Set and save that changes a gain is written to flash, and so is every setpoint change while the schedule is on (only when the gains differ). Set gains when they change, not in a loop.

To try it without the board, run `node simulator/tests/tcp-board.js` and use `127.0.0.1` as the address.
