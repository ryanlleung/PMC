# Control test GUI

`pmc_gui.py` is a small web server on a PC that talks to the board's text port (TCP 8888), and to a Lake Shore 336 (TCP 7777) when one is given, and serves one page to a browser. It holds the board's one connection, so every tool for the running board is on that page.

## The page

- **Top bar**, on every tab: the tabs, then three status lights and the board's mode.
  - Board: connected, or why there is no link.
  - Lake Shore: the temperatures, off, or why there is no link.
  - Logger: the rows written so far (red dot while it records), or Not logging.
- **Run bar**, under it: the test that is running.
  - Its steps: done (with the result, such as KP and TI, and how long it took), in progress, still to do, skipped, stopped.
  - The stage it is in, how long it has been in it and the most it can take.
  - Elapsed time, time left (about, for the gain table), the clock time it should finish, a progress bar, and the folder it saves to.
  - Stop test. When nothing runs, it shows how the last test ended.
- **Readings and charts**, on every tab: pressure, setpoint and valve, and each 336 input by name. The pressure chart (pressure and setpoint) marks where each test stage starts; the valve has its own chart and scale under it, so a move of a few tenths of a percent shows. Each temperature has its own strip and scale under it, so a few mK of change shows. 5 min to 4 hours.
- **STOP**, on every tab: the valve stops where it is and any test ends, with nothing put back. Stop test, by contrast, puts back the setpoint and mode the test found.
- **Every button says what happened**: it flashes green or red, and a line at the bottom of the page names the action and the board's answer. The mode buttons and the gain schedule On / Off show the board's state, filled when on. A PID box changed but not yet sent has an amber edge. While a test runs, its card is outlined and marked Running, and the other Run buttons are greyed.

The tabs:

- **Live**: mode (AUTO / MANUAL), setpoint, Go to %; the PID gains in use (`PID?`) and Set and save (`PID SET`, firmware 0.15.8 and later); the logger; the Lake Shore 336 connection; the activity log.
- **Tests**, each with how long it takes from its boxes, run in this order:
  1. Valve step test: MANUAL with the valve at At %, a wait for the pressure to settle, then the valve opened by Step % and closed back, each recorded. Each response is fitted to a gain (mbar per %), a delay and a time constant, and the KP and TI the SIMC rules give for them are shown, with a Use button that puts them in the PID boxes (Set and save then applies them). Start At near where AUTO holds the setpoint you use; a larger delay closing than opening is the drive's backlash.
  2. Setpoint step test, with the gains chosen: a list of setpoints, `1 2 3 5 8 12 18 25 50` unless changed (the gain table's), and back down through them when ticked. AUTO at the first until it holds within the band for 1 minute (after 10 minutes it steps anyway), then a step to each next one. A step ends once the pressure has held within 5 % of the step (or the band, if larger) for 1 minute, or at the Limit (300 s unless changed). Each step's row gives the KP and TI in use, the time to 90 %, the overshoot, the settling time and the last minute's mean and spread. Two setpoints give a single step. To check a gain table, load it and turn the gain schedule On first: each step then starts with the table's gains for its new setpoint.
- **Gains**: the gain schedule from `tools/gain-schedule/pmc_gains.py`, without its command line. Firmware 0.15.15 and later has its own gain schedule, the PID table (three PID sets, Settings > PID tuning > PID table): while that is on, the PID box line names the set in use and this page's schedule turns itself off, since its PID SET would overwrite the set in use.
  - Autotune the PID table, at 1 2 4 8 12 16 24 48 mbar unless changed (`tools/gain-schedule/pmc_autotune.py`). It finds the board's three PID sets by experiment, as an industrial autotuner does. Before it starts it checks that the valve is homed, there is no fault and Remote control is On. Start it with AUTO holding a mid setpoint. Then, lowest setpoint first:
    1. AUTO at the setpoint until the last minute's mean is on it and level (10 minutes at most), with the First gains at the first setpoint (KP 0.10, TI 0.40 unless changed) and the gains found at the setpoint before for each later one. The valve position it holds at is x0.
    2. Relay test, in MANUAL: the valve goes to x0 + d while the pressure is below the setpoint and x0 - d while it is above, so the loop oscillates by itself. The size of that oscillation gives the ultimate gain Ku (the KP at which AUTO would just oscillate) and its period Pu. d starts at the Relay step box for the first setpoint, then from the Ku before; it doubles when nothing crosses for 90 s (after first moving x0 by d, in case x0 was off) and halves when the swing passes 15 % of the setpoint. If the pressure runs off (over twice the setpoint, or under 40 % of it) d drops to a quarter and the test starts again; only at the least d (0.1 %) is the setpoint skipped. It ends after two steady cycles, or 12 minutes.
    3. KP = Ku / 4 (the KP = Ku / box), TI = 2 Pu, KD 0.
    4. Check, in AUTO with those gains: the setpoint 20 % up and back. Overshoot over 10 % or more than two reversals: KP x 0.8; no overshoot and not settled within 6 Pu: KP x 1.25; then the check again. Trims up stop after two, or after any trim down; trims down go on until the check passes (six cuts at most).

    Whenever AUTO runs (reaching and holding a setpoint, the check steps) a watchdog watches for AUTO oscillating hard: four swings in a row within 400 s, each more than 30 % of the setpoint (0.2 mbar at least). It then cuts KP at once with `PID SET`, by half, or to a quarter when the swing is bigger than the setpoint itself, and goes on with the lower gain; a check step that oscillated is tried again.

    A setpoint that cannot be reached, gives no usable oscillation, or where the board leaves AUTO (a warning) is skipped; a fault ends the run. 15 to 30 minutes a setpoint (the longer with trims), so 2 to 4 hours for eight. While it runs the PID table is Off and set 1 holds the gains under test (`PID SET`); at the end set 1, the PID table, the setpoint and the mode are put back as found.

    The Working card at the top of the Gains tab shows the sums as they are done, under a heading for each setpoint: x0, the noise and switch band, each relay cycle, Ku and Pu with the numbers that went in, KP and TI, each check step and the rule it triggered, any watchdog cut, and the result. A line above it shows the stage running now (time so far, pressure, watchdog count). The same lines go to `working.txt` in the run folder. Under the PID table, each set's geometric mean is written out.

    The results fill in as each setpoint finishes: per setpoint Ku, Pu, the relay step, KP, TI and the check (with any trims), and the three PID table sets, each the geometric mean of the setpoints in its band (below B1, B1 to B2, above B2; 6 and 20 mbar unless changed). A band with no setpoint measured keeps the board's set. Write to board sends `PIDS SET 1` to `3` and `SCHED SET PV,<B1>,<B2>,<H>`, which turns the PID table on; nothing is written before that. Firmware before 0.15.15 has no PID table: load the run's `table.csv` as the gain table below instead. The relay sees the valve as it is at that setpoint, needle stiction included, which the step fits below cannot.
  - Make a gain table, at 1 2 3 5 8 12 18 25 50 mbar unless changed: at each setpoint, AUTO until the pressure holds, then MANUAL valve steps up and back, fitted to a KP and TI (`pmc_gains.py tune`). 10 to 20 minutes a setpoint. A setpoint that gives no usable step, or where the board leaves AUTO (a fault or warning), is skipped and the run goes on to the next. At the end it puts back the setpoint and mode it found, with the valve where the last setpoint held.
  - Download the table as CSV, or load one (from here or from `pmc_gains.py tune`).
  - The KP and TI the schedule uses at 1 to 50 mbar, blended log-log between rows.
  - Gain schedule On: each setpoint change sets KP and TI from the table (`pmc_gains.py run`), also during setpoint step tests, so a step test shows the scheduled gains. Paused while a table is made.
- **Board**: the board's identity, Reset faults and OK warning, the event log (`EVNT?`), the board clock set to the PC's time (`TIME SET`), and a command line for any text-port command.

The step fit, the SIMC rules, the table blend and the tune sequence are the ones in `pmc_gains.py`, imported from there, so the page and the command line give the same numbers. The autotune's relay test, rules and sets are in `pmc_autotune.py` beside it.

Not on the page: the firmware's own checks (`simulator/tests`, `tools/*-test`, `tools/xbuild`, `tools/ui-preview`). They build the firmware with a compiler or the simulator with Node and never talk to a board.

## What is saved

Everything is written as it happens, each row flushed to disk, in `pmc-logs` in the folder the GUI is started from (`--log-dir` to change it; the Logger box shows the full path).

- `pmc_<date>_<time>.csv`, the logger: every sample, one a second, with the PC time, the setpoint, pressure, valve, mode, alarms, the test stage, and each 336 input in K (`A_heat_exchanger_K`, `D_cold_head_K`). It records whether or not a test runs, and keeps recording the temperatures when the board's link is down. Stop logging, Start logging and New file split it; a new file also starts when the 336 inputs change.
- One folder per test, `valve_`, `setpoint_`, `gains_` or `autotune_<date>_<time>`:
  - `samples.csv`: the samples while the test ran, as in the logger.
  - `messages.txt`: every message the test gave, with the time.
  - `results.csv`: each step's result (valve and setpoint tests).
  - `table.csv`: the gain table, rewritten after every setpoint; `steps.csv`: each valve step's fit as soon as it has one; `sp<mbar>_up.csv` and `sp<mbar>_down.csv`: each valve step's raw data (gain table).
  - Autotune: `results.csv`, one row per setpoint (Ku, Pu, relay step and swing, KP, TI, trims, the check); `sets.csv`, the three PID table sets; `write-to-board.txt`, the lines Write to board sends, to paste into PuTTY if wanted; `working.txt`, the Working card's lines with the PC time; `table.csv`, the rows as a gain table; `relay_<mbar>mbar.csv` and `check_<mbar>mbar_<n>_up.csv` / `_back.csv`, each relay test's and check step's samples.

So a test that is stopped, stalls, loses the link or is cut short by closing the GUI keeps everything up to there. The run bar says which step it ended in and what it was doing.

Download session CSV (Logger box) gives the samples since the GUI started, up to 4 hours, from memory.

Long logs (a weekend): the GUI's memory does not grow (the page keeps 4 hours, the event log 200 lines) and the logger file grows about 0.3 MB an hour, so 70 hours is one file of about 20 MB and 250,000 rows, which Excel opens. What stops a log is the PC sleeping. On Windows the GUI asks the PC not to sleep on its own while it runs (`--allow-sleep` turns that off); the screen can still turn off. Closing the lid, Sleep from the Start menu or a Windows Update restart still stop it, so keep the laptop plugged in with the lid open and restart for any pending update before you leave. Closing the browser tab does not stop the log; closing the GUI's window does. A gap of over a minute with no samples is written to the event log with its start and end, so a gap shows on Monday.

## Run it with Python

Python 3.8 or later, standard library only. Keep `tools/pmc-gui` and `tools/gain-schedule` side by side (it imports the step fit from there), so nothing needs pip or admin rights. On Windows without admin, the "Windows embeddable package" zip from python.org runs after unzipping.

```
python pmc_gui.py <board IP>
python pmc_gui.py <board IP> --ls336 <336 IP>
```

Then open the address it prints, normally http://localhost:8080. If Windows refuses 8080 (WinError 10013: the port is in a block Windows reserves for Hyper-V, WSL or Docker) or another program has it, the GUI tries the next ports and prints the one it got. `--port 8090` picks a page port yourself.

The 336 can also be connected from the page (Live tab, Lake Shore 336: its IP, then Connect). Its inputs default to `A=Heat exchanger, D=Cold head`; give others as `--ls-inputs A "B=Sample" D`, or in the Inputs box. The 336 needs Ethernet on (Interface menu) with an address on the PC's subnet. `tools/lakeshore/ls336_log.py` logs the 336 alone, without the GUI.

## Before you start

- Settings > System setup > Remote control On, or every button answers ERR 211.
- If the board's link drops, the page says so in the top bar and reconnects every 2 s by itself. A gain table run waits up to 2 minutes for the link to come back (the run bar says Waiting) and then carries on; after that it stops and says so. The board itself carries on in the mode it was in (a run that stops mid-step leaves the valve in MANUAL where it was).
- The board takes one text-port connection at a time, and a new one replaces the old. Close PuTTY, `pmc_gains.py` and `sp_step.py` first. The 336 logger can run alongside: it is a separate connection to a separate instrument.
- STOP on the page and on the screen both end AUTO. If the board leaves AUTO during a setpoint step test (a fault, W-2, W-4, W-5 or STOP), the test ends and says why.
- A fault (F-) ends a gain table run, since the board refuses AUTO and valve moves until it is reset: reset it on the Board tab and start the run again with the setpoints left. A warning that drops the board to MANUAL (W-2, W-4, W-5) skips that setpoint only.
- Every Set and save that changes a gain is written to flash, and so is every setpoint change while the schedule is on (only when the gains differ). Set gains when they change, not in a loop.

To try it without the board, run `node simulator/tests/tcp-board.js` and use `127.0.0.1` as the address.
