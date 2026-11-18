# Lake Shore 336 temperature log

`ls336_log.py` reads temperatures from a Lake Shore Model 336 over Ethernet and writes them to a CSV, one row per reading: the PC's date and time, seconds from the start, and one column per input in kelvin, named after the input and what it measures (`A_heat_exchanger_K`, `D_cold_head_K`).

```
python ls336_log.py <336 IP>                                     # A heat exchanger, D cold head, every 1 s
python ls336_log.py <336 IP> --inputs A "B=Sample" D --every 5 --out run1.csv
```

- Python 3.8 or later, standard library only. The 336 and the controller are separate connections, so it can run while the controller's text port is in use.
- Inputs without a name use the defaults: A heat exchanger, D cold head; others are "Input B" and so on.
- It talks to TCP port 7777, the 336's Ethernet command port, with `KRDG?` (kelvin) for each input.
- The 336 needs Ethernet on (Interface menu) with an address on the PC's subnet.
- If the link drops it says so, reconnects every 5 s and carries on. Ctrl+C stops it.
- A reading the 336 cannot give (sensor open, over range) is left empty.
- The file name defaults to `ls336_<date>_<time>.csv` in the folder you run it from. The PC clock is in the first column.
