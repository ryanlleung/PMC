# Lake Shore 336 temperature log

`ls336_log.py` reads temperatures from a Lake Shore Model 336 over Ethernet and writes them to a CSV, one row per reading: the PC's date and time, seconds from the start, and one column per input in kelvin.

```
python ls336_log.py <336 IP>                                  # inputs A and D, every 1 s
python ls336_log.py <336 IP> --inputs A B C D --every 5 --out run1.csv
```

- Python 3.8 or later, standard library only. It runs from the same Miniconda prompt as the control test GUI, and alongside it: the 336 and the controller are separate connections.
- It talks to TCP port 7777, the 336's Ethernet command port, with `KRDG?` (kelvin) for each input.
- The 336 needs Ethernet on (Interface menu) with an address on the PC's subnet.
- If the link drops it says so, reconnects every 5 s and carries on. Ctrl+C stops it.
- A reading the 336 cannot give (sensor open, over range) is left empty.
- The file name defaults to `ls336_<date>_<time>.csv` in the folder you run it from. The PC clock is in the first column, so the rows line up with the GUI's CSV by time.
