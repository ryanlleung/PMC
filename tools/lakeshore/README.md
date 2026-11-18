# Lake Shore 336 temperature log

`ls336_log.py` reads temperatures from a Lake Shore Model 336 over Ethernet and writes them to a CSV file.

## Run

```
python ls336_log.py <336 IP>
python ls336_log.py <336 IP> --inputs A "B=Sample" D --every 5 --out run1.csv
```

The first line logs input A (heat exchanger) and input D (cold head) once a second. Ctrl+C stops it.

| Option | Meaning | Default |
| --- | --- | --- |
| `--inputs` | The inputs to read, each with an optional name: `A`, `"B=Sample"` | A heat exchanger, D cold head. An input without a name is "Input B" and so on |
| `--every` | Seconds between readings | 1 |
| `--out` | The CSV file | `ls336_<date>_<time>.csv` in the folder it is run from |

## Needs

- Python 3.8 or later. Standard library only.
- Ethernet turned on at the 336 (Interface menu), with an address on the PC's subnet.

The 336 and the controller are separate connections, so the log can run while the controller's text port is in use.

## The file

One row per reading:

| Column | Holds |
| --- | --- |
| First | The PC's date and time |
| Second | Seconds from the start |
| One per input | The temperature in kelvin. The column is named after the input and what it measures: `A_heat_exchanger_K`, `D_cold_head_K` |

A reading the 336 cannot give (sensor open, over range) is left empty.

## How it talks to the 336

It connects to TCP port 7777, the 336's Ethernet command port, and sends `KRDG?` for each input. If the link drops it says so, reconnects every 5 s and carries on.
