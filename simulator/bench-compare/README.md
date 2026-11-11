# Simulator against the bench

The simulator's bench cryostat (`../wasm/bench.c`) driven with the valve moves from a bench session log, drawn over that log.

```
node bench-replay.js pmc_session_<date>_<time>.csv [from_s] [to_s] > replay.csv
python3 plot_bench.py replay.csv out.png "title" [--log] [--from S] [--to S] [--seconds]
```

`bench-replay.js` first holds the log's first pressure in AUTO for 30 min, so the simulator starts where the bench did, heat exchanger included. Then it goes to MANUAL, and its valve follows the logged valve second by second, plus a fixed offset (its own valve for that first pressure less the bench's, printed). This is open loop: the same valve moves, and you see what the pressure and the heat exchanger do on each. `plot_bench.py` needs matplotlib.

| Plot | Log |
|---|---|
| `1110-10-to-1-mbar.png`, `1110-10-to-1-mbar-first-5-min.png` | 10 Nov: 10 to 1 mbar reach at KP 1.20, the 1 mbar relay, the start of the 2 mbar reach |
| `1109-1-mbar.png` | 9 Nov: 1 mbar autotune, then the 2 mbar reach that swung |
| `1109-2-to-16-mbar.png` | 9 Nov: 2 to 16 mbar autotune |
| `1109-16-and-24-mbar.png` | 9 Nov 16:23: 16 and 24 mbar autotune |
| `1111-step-test.png` | 11 Nov 17:26: the GUI setpoint step test, each step against the simulator's step test with the table as run (0.16.12) and with the proposed table (0.16.13) |
| `1111-step-test-4sets.png` | The same bench run against the simulator's step test with four sets (0.16.14, B1 6, B2 10, B3 14 mbar, H 10 %, set 1 0.06/0.70, set 3 0.50/1.00, set 4 0.30/0.25): set 2 0.14/1.50 (12 to 8 mbar ends 8.55 after 6 min) and set 2 0.08/0.80 (settles in 209 s) |

For a GUI step test, `step_figures.py` works out each step's figures from the session CSV (rise 10 to 90 %, settling into 5 % of the setpoint, overshoot, end error over the last 30 s, the valve's kick in the first 3 s), and `plot_step_test.py` puts each step of the bench and of simulator runs of the same steps side by side:

```
python3 step_figures.py pmc_session_<date>_<time>.csv figures.json
python3 plot_step_test.py out.png pmc_session_<date>_<time>.csv "sim=pmc_<sim run>.csv" [--title="..."]
```

The session logs themselves are not in the repository.
