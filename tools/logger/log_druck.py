#!/usr/bin/env python3
"""Log the board's pressure, target, valve, mode and alarms to CSV, once a second.

    python log_druck.py 192.168.1.23         # Ethernet build: IP shown on the board
    python log_druck.py COM3                 # USB build
    python log_druck.py 192.168.1.23 -o run1.csv

Writes druck_YYYYMMDD_HHMMSS.csv unless -o is given. Stop with Ctrl+C.
Ethernet: TCP port 8888 (give host:port for another). Needs firmware 0.16.10
or later: the command names are whole words from there (PRESSURE?, not PRES?).
The board takes one connection at a time; a new one replaces the old, and
the logger then reconnects on its own. USB: close the NECTO UART Terminal
first, only one program can hold the COM port. Needs pyserial either way
(pip install pyserial).

The board sends nothing unasked (0.13.0), so the logger polls one line a
second, PRESSURE?;SETPOINT?;VALVE?;MODE?;ALARM?;BAROMETER?, and sets the board clock
from the PC at connect. Columns: pc_time (local, ISO), p_mbar, target_mbar,
valve_pct, mode, alarm (the active tags, spaces between), baro_mbar. An
empty field has no value (no reading, not homed, no target, no barometer).
"""
import argparse
import datetime as dt
import re
import sys
import time

import serial   # pip install pyserial

TCP_PORT = 8888
POLL = "PRESSURE?;SETPOINT?;VALVE?;MODE?;ALARM?;BAROMETER?"
# The words each reply starts with, in POLL order.
WORDS = ["PRESSURE", "SETPOINT", "VALVE", "MODE", "ALARM", "BAROMETER"]
FIELDS = ["p_mbar", "target_mbar", "valve_pct", "mode", "alarm", "baro_mbar"]


class Link:
    """Line-based link to the board: a COM port, or TCP to host[:port].

    pyserial opens both (TCP through its socket:// URL), so reads and
    writes are the same either way.
    """

    def __init__(self, target, timeout=3.0):
        if not re.match(r"^(COM\d+|/dev/)", target, re.I):
            host, _, port = target.partition(":")
            target = f"socket://{host}:{port or TCP_PORT}"
        self.port = serial.serial_for_url(target, baudrate=115200, timeout=timeout)
        self.port.dtr = True    # baud and DTR are ignored over TCP
        idn = self.ask("*IDN?")
        if not idn.startswith("MercShore,iPC,"):
            raise ConnectionError(f"not an mm4 with firmware 0.16.10 or later (*IDN? gave {idn!r})")
        # The command names are whole words from 0.16.10 (PRESSURE?, not PRES?).
        version = tuple(int(n) for n in re.findall(r"\d+", idn.split(",")[-1])[:3])
        if version < (0, 16, 10):
            raise ConnectionError(f"firmware {idn.split(',')[-1]} has the old command names: "
                                  "flash 0.16.10 or later, or use the logger from that firmware's commit")
        print(idn)
        # Board clock from the PC's local time; the TIME? query makes one reply line.
        print(self.ask(time.strftime("TIME SET %Y-%m-%d %H:%M:%S;TIME?")))

    def ask(self, line):
        """Send one line, return its one reply line ("" after the timeout)."""
        self.port.write((line + "\n").encode("ascii"))
        return self.port.readline().decode("ascii", errors="replace").strip()

    def close(self):
        self.port.close()


def value(reply, words):
    """The value of one reply ("PRESSURE 4.625 mbar" -> "4.625"), "" for NONE."""
    if not reply.startswith(words + " "):
        raise ValueError(f"expected {words} ..., got {reply!r}")
    v = reply[len(words) + 1:]
    if v == "NONE":
        return ""
    if words == "ALARM":
        return " ".join(a.split(",")[0] for a in v.split("|"))
    return v.split(" ")[0]


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
            # it, a cable was pulled, the board reset) or stops answering.
            while True:
                try:
                    port = Link(a.target)
                except (ConnectionError, OSError) as e:
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
    next_t = time.time()
    while True:
        next_t += 1.0
        reply = port.ask(POLL)
        if not reply:
            raise ConnectionError("no reply for 3 s")
        parts = reply.split(";")
        try:
            if len(parts) != len(WORDS):
                raise ValueError(reply)
            vals = [value(r, w) for r, w in zip(parts, WORDS)]
        except ValueError as e:
            print(f"\nskipped: {e}", file=sys.stderr)
            vals = None
        if vals:
            now = dt.datetime.now().isoformat(timespec="milliseconds")
            f.write(now + "," + ",".join(vals) + "\n")
            f.flush()
            rows += 1
            p, v, mode, alarm = vals[0] or "----", vals[2] or "--", vals[3], vals[4]
            print(f"\r{rows:6d}  {p:>8} mbar   valve {v:>6} %   {mode} {alarm}      ", end="", flush=True)
        time.sleep(max(0.0, next_t - time.time()))


if __name__ == "__main__":
    main()
