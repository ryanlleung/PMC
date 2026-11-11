# Autotune for the PID table

`pmc_autotune.py` holds the autotune that the control test GUI (`tools/pmc-gui`) runs from its Gains tab. It has no command line of its own: run it from the GUI. It also holds the text-port link, the clock and the sampling the GUI's runs stand on.

A single KP and TI does not suit every setpoint, because the pressure moves several times more per % of valve at 1 mbar than at 12 mbar, and the response is slow where the heat exchanger holds liquid and fast where it holds gas. The firmware (0.15.15 and later) has three PID sets and a PID table that picks the set from the pressure. The autotune finds the sets by experiment.

Pressure here is a rough stand-in for helium flow: it is measured and tuned as pressure, in mbar.

## Needs

- Python 3.8 or later, standard library only. Nothing is installed with pip. On a Windows PC without admin rights, the "Windows embeddable package" zip from python.org runs after unzipping.
- Firmware 0.16.12 or later (the text port names are whole words from there: `PRESSURE?`, `SETPOINT`, `VALVE SET`), and Menu > System > Remote control On.
- The board takes one text-port connection at a time, and a new one replaces the old: close PuTTY and the logger first.

## The procedure, per setpoint

The setpoints run in the order given. Not benched yet.

1. **Reach.** AUTO at the setpoint with safe gains (the point's learned gains if it has any) until it holds: the minute's mean within 5 % of the setpoint, the last 30 s within 2.5 % and within 1.25 % of the 30 s before. The valve position it holds at is x0. (Before 11 Nov the 5 % alone counted, and the bench called 24 mbar held at 23.07 and 48 at 45.85 while still rising.)
2. **Relay test** (Astrom and Hagglund), in MANUAL: the valve at x0 + d while the pressure is below the setpoint and x0 - d while it is above, so the loop oscillates by itself. The oscillation gives the ultimate gain Ku (the KP at which AUTO would oscillate) and its period Pu. d grows when nothing crosses and shrinks when the swing is too big.
3. **First gains.** With a learned fit of the response for this setpoint (K, L, tau, below):

   ```
   L_eff = max(L, Pu / 4)                    seconds
   TI    = min(max(tau, 2.5 L_eff), 20 L_eff)  seconds
   KP    = min(TI / (5 K L_eff), Ku / 3)     % per mbar
   KD    = 0
   ```

   With no fit yet: KP = Ku / 4 and TI = 2 Pu. Either is used only for the first check. A learned fit is not used when the new Ku is more than 2 times off the learned one (the plant has changed).
4. **Check, fit, refine.** A check is AUTO with the gains under test: a hold, then the setpoint 20 % up, then back.
   - **Fit.** After every check the pressure is fitted to the valve over that check's samples (the hold, the step up and the step back, one sample a second):

     ```
     p(t) = c + drift x t + K x lag_tau( valve(t - L) )
     ```

     L on 0, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64 s and tau on 2, 4, 6, 9, 13, 18, 25, 35, 50, 70, 100, 140, 200 s. For each pair the valve is delayed and put through a first-order filter, and c, drift and K come from least squares (the normal equations). The pair with the least squared error is kept. A fit is accepted when R2 is 0.8 or more, K is above 0 and the valve moved more than 0.05 % in the window. Otherwise the previous fit is kept, or none. On the 35 check windows of the 10 Nov bench logs it gives exactly what the numpy version (`bench-data/ml-fit/gains-harness/fopdt.py`) gives.
   - **Figures**, for each leg (up and back); the check takes the worse of the two:
     - overshoot beyond the pressure's own wander, as % of the step, and error reversals;
     - t63 and t90: seconds to cover 63 % and 90 % of the step;
     - back-off: the largest retreat of the valve from its furthest point before t90, as % of the valve's whole move in that leg (the furthest it got from where it started, so never over 100 %). This is the "goes open more, then stops, then goes open slowly again" shape;
     - sag: the longest time, once the pressure has reached the band, that it is under the band (short of the setpoint, on the way back down as well as up) with the valve not moving (under 0.01 % in 30 s). It is shown but no refine rule uses it yet.
   - **Refine**, the first rule that applies, then check again:

     | | When | Change |
     |---|---|---|
     | 1 | more than 2 reversals, or the watchdog cut KP | KP x 0.7 (a leg the watchdog ended: its own x 0.5 or x 0.25 if that is more) |
     | 2 | overshoot over 5 % of the step, on either leg | KP x 0.8 |
     | 3 | tail: no overshoot (under 1 %), t90 over 4 x t63 and back-off over 10 % | TI x 0.7, KP kept |
     | 4 | slow: no overshoot, back-off 10 % or less, t90 over 8 x (L_eff + tau) | TI x 0.8 |
     | 5 | otherwise | the check passes |

     Rules 3 and 4 never take TI under 2.5 L_eff (in minutes) or the integral gain KP / (60 TI) over 1 / (4 K L_eff); at that limit the check passes with the note "as fast as the cryostat allows". Rules 1 to 4 never raise KP, and never take it over Ku / 3.
   - **Gains from the check as it ran** (`model_step`, `model_gains`; added 11 Nov after the bench run left KP low and TI long). When rule 1 or 2 does not apply, the check is fitted as it ran (`fit_closed_loop`, as for the predicted line below) and, if that fit is within 15 % of the step RMS, KP and TI are searched on three models of it: the fit with its dead time made L_eff; the same with K 1.3 times lower; and K 1.3 times higher with the dead time 4 s longer. The search keeps the gains with the shortest worst settle over the three models, both steps, with no model over 5 % overshoot or 2 reversals. KP runs from Ku / 2 down (never over a KP that rule 1 or 2 cut at this setpoint); TI from its floor (2.5 L_eff, and the integral cap above) up to 3 min. The gains are used, in place of rule 3 or 4 or a pass, when they settle in 0.75 times the check's settle or less (any time, if the check did not settle) and KP or TI differs by more than 15 %. This is the only place KP can go up. It counts as a refinement (the trims say `fit KP .. TI ..`), and is undone like the rules if the next check is worse; a check after it that overshoots cuts KP by rule 2 as usual.
   - At most 4 refinements per setpoint. A refinement that makes the check worse (it oscillates, goes 15 % further over the overshoot or reversal limit, or takes 1.5 times as long to reach 90 %) is undone and ends the refinements, and the earlier gains stand. After each check the fit is redone and the newest accepted fit is kept.
5. **Out of reach.** A check step up ends when the valve is over 90 % for 30 s with the pressure short of the band, or when it runs out of time short of the band with the pressure up less than the band in its last 60 s and the valve opened 15 % or more since the step. The setpoint goes back to the point and the point is skipped, so it is left out of the sets (11 Nov bench: at 48 mbar the pressure stopped at 54.5 mbar with the valve at 98 %, short of 57.6, and five checks ran out of time and cut TI).
6. **Watchdog.** Whenever AUTO runs (reaching, holding, the checks), four swings in a row, each over 30 % of the setpoint, cut KP at once (x 0.5, or x 0.25 when the swing is larger than the setpoint) and AUTO carries on with the lower gain.

## The sets

The PID table sets come from the setpoints in each band: below B1, B1 to B2, B2 to B3, above B3 (four sets, firmware 0.16.14; B3 0 gives three, set 3 above B2). B1 is 6 mbar, B2 10, B3 14 and the hysteresis 10 % unless changed. For each band KP is the lowest KP of the setpoints in it, so no setpoint gets more KP than it was tuned for, and TI is the geometric mean of their TIs. KD is 0. A band with no setpoint keeps the board's set. Write to board in the GUI sends `PIDS SET 1` to `4` and `SCHED SET PV,<B1>,<B2>,<B3>,<H>` (firmware 0.16.14 or later); nothing is written before that.

While it runs the PID table is Off and set 1 carries the gains under test; at the end set 1, the PID table, the setpoint and the mode are put back as found.

## Setpoint step test

`StepTest` (the GUI's Setpoint step test card; Ryan, 11 Nov: "a step test with going from 8 to 48 and back to 2 and back to 8 mb", through the autotune's setpoints) runs AUTO through a list of setpoints, 8 12 16 24 48 24 16 12 8 4 2 4 8 mbar by default. The GUI first writes the PID table in its card (`table_lines`; by default `STEP_SETS`: set 1 KP 0.06 TI 0.70, set 2 KP 0.12 TI 0.83, set 3 KP 0.80 TI 0.94 (from 0.16.15, gains the bench has run at 8 and 12 mbar; before, 0.08/0.80 and 0.50/1.00), set 4 KP 0.30 TI 0.25, KD 0, B1 6, B2 10, B3 14, H 10 %), or leaves the board's; after that only the setpoint is changed. It holds the first setpoint until the pressure is within 5 % of it (0.1 mbar at least) for 60 s (10 min at most), then steps to each next one, each until it holds the same way or `most` (20 min). Each step's figures are `step_figures` with that band, plus the PID set and the KP and TI in force at the end of the step (`PID?`, `PIDS?`), the overshoot in mbar, and the pressure and valve at the start and end.

## The log

Each run writes `autotune_log.csv` in its folder as it goes (`log_row`, `LOG_COLS`): one row per calculation (the Working card's lines), decision (learned or fresh, first gains, each rule, an undone refinement, a watchdog cut, a relay step change, the result), change of gains on the board (`PID SET`, with old -> new and why) and command (setpoint, mode, valve). Every row carries the KP, TI and KD on the board after it. The GUI adds the start (settings and what it found), a skipped setpoint, the put back, the end with the gains read back from the board, and Write to board.

## The predicted line

Before each check step `predict_leg` works out how the step should go, one sample a second: the board's PI in position form, as `ctrl.c` has it (KP x error plus the integral, KP / (60 TI) x error a second, no integration past 0 or 100 %, the integral set so the first output is the valve in force; from firmware 0.16.15 KP acts on the pressure only, so the setpoint step moves the integral and the valve does not jump, and the GUI sets `PI_KICK` for an older board), with the KP and TI under test, on the fitted response with the drift left out. It starts from the pressure and valve at the step, and the valve follows at `PRED_SLEW`, 0.4 % a second (the bench valve does 0.28 to 0.62). The deadbands, the reversal hold and the needle's play are left out, so the real valve moves in fewer, larger steps and the pressure wanders round the line by about its noise.

The fit is the learned one for the setpoint when there is one; else the reach, the noise hold and the relay test are fitted as one series, the same way as a check (for the line only, the gains do not use them): the reach has the slow part of the answer and the relay the fast part. If that fit is not good enough, the better of the reach and the relay alone (R2 0.8 or more) is used. That first line can be well off: the relay sees only the fast part of the answer and the reach mostly the slow part. After each check the line is refitted to the check as it ran (`fit_closed_loop`): K, L and tau on the grids (K 0.25 to 53 mbar/%) for which this same predicted line, run from each step's start with that check's gains, comes closest to the pressure measured. It is used for the next check's line when its error is under half the step (`PRED_SHARE_MAX`), else the check's own fit when accepted. On the simulator at 8 mbar it comes within 7 to 9 % of the step RMS, against 14 to 35 % for the first line. With no usable fit there is no line. The prediction's overshoot, settle time, t63 and t90 go in the Working card and the log (`Predicted for check ...`), the line goes to the GUI's charts (`predicted()`, and `predicted_end()` where the step ended), and each check step's CSV has `p_predicted_mbar` and `valve_predicted_pct` beside the measured columns.

## Learned gains

Each setpoint's result is kept in `autotune-learned.csv` in the GUI's log folder: `sp_mbar, kp, ti_min, Ku, Pu_s, d_pct, a_mbar, valve_pct, trims, note, when`, then the fit it ended on and the last check's shape: `K, L_s, tau_s, t63_s, t90_s, backoff_pct`. Files from before the fit load with those columns empty, and such a setpoint starts from Ku / 4 and 2 Pu. With Start from Learned gains (the default), a setpoint learned before reaches with its own learned KP and TI, sizes its relay step from its Ku and starts its first check from the rule on its learned fit. Fresh ignores the file (its results still go in it). With no learned file yet, it is built from the `results.csv` of the earlier autotune runs in the same log folder.

## Tests

```
python test_pmc_autotune.py       # fit, first-gains rule, refine rules, gains from the check as it ran, out of reach, sets, relay, watchdog, the step test and the whole sequence on a valve model (seconds)
```
