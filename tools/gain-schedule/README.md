# Gain schedule

`pmc_gains.py` matches the AUTO loop's KP and TI to the setpoint, from a PC over the text port. A single KP and TI does not suit every setpoint, because the pressure moves several times more per % of valve at 1 mbar than at 12 mbar. A KP that is right at 10 mbar swings at 2 mbar, and one that is right at 2 mbar is slow at 10 mbar.

The program has two parts:

- `tune` measures the plant at a few setpoints with step tests and writes a table of KP and TI, one row per setpoint.
- `run` stays connected, and each time the setpoint changes it writes the KP and TI blended from the table for that setpoint.

## Needs

- Python 3.8 or later. Nothing is installed with pip. On a Windows PC without admin rights, the "Windows embeddable package" zip from python.org runs after unzipping (`python.exe pmc_gains.py ...`).
- The Ethernet build, with Settings > System setup > Remote control On.
- `tune` works on firmware 0.13.0 and later. `run` needs the `PID?` and `PID SET` commands, from firmware 0.15.8.

The board takes one text-port connection at a time, and a new one replaces the old. Do not run `run` beside the logger or a Frappy driver on the same board. A Frappy driver can import `load_table()` and `gains_for()` from this file and set KP and TI itself.

## Making the table for a cryostat

1. Run the cryostat as for an experiment: pot filled, pump on, and the sample heater at its usual power, because the gains change with the heater.
2. Pick three setpoints that span the range used on that cryostat, for example 2, 5 and 10 mbar. Put the lowest first.
3. Run:

   ```
   python pmc_gains.py tune 192.168.1.100 --sp 2 5 10 --out cf11.csv
   ```

   At each setpoint it runs AUTO until the pressure holds within 3 % for 2 minutes, then switches to MANUAL at the valve position AUTO held. It then steps the valve up by `--step` % (0.5 by default) and back down, and fits a gain, a delay and a time constant to each response. A step that moves the pressure less than 5 % of the setpoint is doubled, and one that moves it more than 30 % is stopped, put back and halved. Each setpoint takes about 10 to 20 minutes. At the end, and on Ctrl+C, it puts back the valve position, setpoint and mode it found.

4. Check the result:

   ```
   python pmc_gains.py show cf11.csv
   ```

   This lists the measured rows, then the KP and TI that `run` would use from 1 to 25 mbar. The CSV can be edited by hand. Only the `sp_mbar`, `kp` and `ti_min` columns are used. KP is in % per mbar and TI in minutes, as on the PID tile.

KP and TI come from the SIMC rules: KP = tau / (K (tc + delay)) and TI = min(tau, 4 (tc + delay)), with tc equal to the delay. `--tc 1.5` gives a calmer loop and `--tc 0.7` a faster one. The recorded steps are kept as CSV files in `cf11_steps/`, and `python pmc_gains.py fit cf11_steps/sp5_up.csv` refits one of them.

## Running it

```
python pmc_gains.py run 192.168.1.100 cf11.csv
```

It polls `PRES SP?;MODE?;PID?` every 2 s. When the setpoint changes, and once at connect, it writes the blended KP and TI with `PID SET`, which does what the Home PID tile does, and prints one line. Between the table's rows the values are blended log-log on the setpoint. Below the first row and above the last, the end row is used. A KP or TI typed on the screen stays until the next setpoint change. `--dry-run` prints what it would set and sets nothing.

If the PC or the network stops, the board keeps the last KP and TI it was given. `run` reconnects on its own.

## Checking the loop: setpoint steps

`sp_step.py` checks how AUTO answers a setpoint change with the KP and TI on the board. Use it to compare gains, or to check a table from `tune`.

```
python sp_step.py 192.168.1.100 --start 3 --to 5 --back --out cf11_3to5.csv
```

It starts AUTO at `--start`, waits until the pressure holds within `--band` (0.1 mbar) for 2 minutes, then steps the setpoint to `--to` and records for `--record` s (600 by default). With `--back` it steps back and records again. It writes one line a second to the CSV and prints, for each step, the time to 90 % of the step, the overshoot, the settling time and the mean and spread of the last minute. Ctrl+C puts the setpoint back to `--start` and leaves AUTO running.

## Autotune for the PID table

`pmc_autotune.py` holds the autotune that the control test GUI runs from its Gains tab (Autotune the PID table; the GUI's README describes it). At each setpoint a relay test in MANUAL measures the ultimate gain Ku and period Pu, KP = Ku / 4 and TI = 2 Pu, and a setpoint step up and back trims KP. Whenever AUTO runs, a watchdog cuts KP at once (by half, or to a quarter) when the pressure oscillates hard: four swings in a row, each over 30 % of the setpoint. Each sum is passed to the GUI as a line of working. The rows are then combined into the firmware's three PID sets. It has no command line of its own: run it from the GUI.

## Tests

```
python test_pmc_gains.py          # fit, table and run against a fake board
python test_pmc_gains.py --sim    # also tunes 3 mbar and runs on simulator/tests/tcp-board.js (needs node)
```

```
python test_pmc_autotune.py       # relay test, rules and the whole sequence on a valve model (seconds)
```
