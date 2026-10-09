#!/usr/bin/env python3
"""Log the board's once-a-second DATA lines to CSV, over USB or Ethernet.

    python log_druck.py 192.168.1.23         # Ethernet build: IP shown on the board
    python log_druck.py COM3                 # USB build
    python log_druck.py 192.168.1.23 -o run1.csv

Writes druck_YYYYMMDD_HHMMSS.csv unless -o is given. Stop with Ctrl+C.
Ethernet: TCP port 5000 (give host:port for another). The board takes one
connection at a time; a new one replaces the old, and the logger then
reconnects on its own (so running druck_cal.py only leaves a short gap).
USB: close the NECTO UART Terminal first, only one program can hold the
COM port. Needs pyserial either way (pip install pyserial).

Columns: pc_time (local, ISO), then the board's fields:
t_ms, p_mbar, signal_uV, exc_mV, ratio_mV_per_V, die_C, set_mV, ok.
Status lines from the board (anything not starting with DATA) are printed
to the console only.
"""
import argparse
import datetime as dt
import re
import sys
import time

import serial   # pip install pyserial

TCP_PORT = 5000

FIELDS = ["t_ms", "p_mbar", "signal_uV", "exc_mV", "ratio_mV_per_V", "die_C", "set_mV", "ok"]


class Link:
    """Line-based link to the board: a COM port, or TCP to host[:port].

    pyserial opens both (TCP through its socket:// URL), so reads and
    writes are the same either way.
    """

    def __init__(self, target, timeout=2.0):
        if not re.match(r"^(COM\d+|/dev/)", target, re.I):
            host, _, port = target.partition(":")
            target = f"socket://{host}:{port or TCP_PORT}"
        self.port = serial.serial_for_url(target, baudrate=115200, timeout=timeout)
        self.port.dtr = True    # baud and DTR are ignored over TCP
        self.buf = b""
        # In case DATA OFF was left set from a terminal session.
        self.write("DATA ON\n")
        # Board clock from the PC's local time.
        self.write(time.strftime("TIME SET %Y-%m-%d %H:%M:%S\n"))

    def readline(self):
        """One line without its ending, or "" if none is complete yet.

        A read that times out mid-line keeps the part it has for the next
        call. A closed TCP connection raises SerialException (an OSError).
        """
        self.buf += self.port.readline()
        if not self.buf.endswith(b"\n"):
            return ""
        line, self.buf = self.buf, b""
        return line.decode("ascii", errors="replace").strip()

    def write(self, text):
        self.port.write(text.encode("ascii"))

    def close(self):
        self.port.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("target", help="board IP (Ethernet) or COM port (USB)")
    ap.add_argument("-o", "--out", help="output CSV file")
    a = ap.parse_args()

    out = a.out or dt.datetime.now().strftime("druck_%Y%m%d_%H%M%S.csv")
    rows = 0
    with open(out, "w", newline="") as f:
        f.write("pc_time," + ",".join(FIELDS) + "\n")
        print(f"Logging {a.target} to {out}  (Ctrl+C to stop)")
        try:
            # Reconnect if the board drops the connection (another tool took
            # it, a cable was pulled, the board reset) or goes quiet for 10 s.
            while True:
                try:
                    port = Link(a.target)
                except OSError as e:
                    print(f"\nconnect failed ({e}), retrying", file=sys.stderr)
                    time.sleep(2)
                    continue
                try:
                    rows = log_rows(port, f, rows)
                except (ConnectionError, OSError) as e:
                    print(f"\n{e}, reconnecting", file=sys.stderr)
                finally:
                    port.close()
                time.sleep(2)
        except KeyboardInterrupt:
            print(f"\nStopped. {rows} rows in {out}")


def log_rows(port, f, rows):
    last = time.time()
    while True:
        line = port.readline()
        if not line:
            if time.time() - last > 10:
                raise ConnectionError("no data for 10 s")
            continue
        last = time.time()
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

if __name__ == "__main__":
    main()
