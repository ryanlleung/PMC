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

The session logs themselves are not in the repository.
