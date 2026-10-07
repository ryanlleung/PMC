#!/usr/bin/env python3
"""Log the board's once-a-second DATA lines from the USB COM port to CSV.

    python log_druck.py COM3                 # writes druck_YYYYMMDD_HHMMSS.csv
    python log_druck.py COM3 -o run1.csv

Needs pyserial (pip install pyserial). Close the NECTO UART Terminal first:
only one program can hold the COM port. Stop with Ctrl+C.

Columns: pc_time (local, ISO), then the board's fields:
t_ms, p_mbar, signal_uV, exc_mV, ratio_mV_per_V, die_C, set_mV, ok.
Status lines from the board (anything not starting with DATA) are printed
to the console only.
"""
import argparse
import datetime as dt
import sys

import serial

FIELDS = ["t_ms", "p_mbar", "signal_uV", "exc_mV", "ratio_mV_per_V", "die_C", "set_mV", "ok"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="COM port, e.g. COM3")
    ap.add_argument("-o", "--out", help="output CSV file")
    a = ap.parse_args()

    out = a.out or dt.datetime.now().strftime("druck_%Y%m%d_%H%M%S.csv")
    # Baud rate is ignored by USB CDC but pyserial needs one.
    with serial.Serial(a.port, 115200, timeout=2) as port, open(out, "w", newline="") as f:
        port.dtr = True
        f.write("pc_time," + ",".join(FIELDS) + "\n")
        print(f"Logging {a.port} to {out}  (Ctrl+C to stop)")
        rows = 0
        try:
            while True:
                line = port.readline().decode("ascii", errors="replace").strip()
                if not line:
                    continue
                if not line.startswith("DATA,"):
                    print(line)
                    continue
                vals = line.split(",")[1:]
                if len(vals) != len(FIELDS):
                    print("skipped malformed line:", line, file=sys.stderr)
                    continue
                now = dt.datetime.now().isoformat(timespec="milliseconds")
                f.write(now + "," + ",".join(vals) + "\n")
                f.flush()
                rows += 1
                p, exc = vals[1] or "----", vals[3]
                print(f"\r{rows:6d}  {p:>10} mbar   exc {exc} mV   ", end="", flush=True)
        except KeyboardInterrupt:
            print(f"\nStopped. {rows} rows in {out}")


if __name__ == "__main__":
    main()
