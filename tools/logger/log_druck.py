#!/usr/bin/env python3
"""Log the board's once-a-second DATA lines to CSV, over USB or Ethernet.

    python log_druck.py 192.168.1.23         # Ethernet build: IP shown on the board
    python log_druck.py COM3                 # USB build
    python log_druck.py 192.168.1.23 -o run1.csv

Writes druck_YYYYMMDD_HHMMSS.csv unless -o is given. Stop with Ctrl+C.
Ethernet: TCP port 5000 (give host:port for another). The board takes one
connection at a time; a new one replaces the old, and the logger then
reconnects on its own (so running druck_cal.py only leaves a short gap).
USB: needs pyserial (pip install pyserial); close the NECTO UART Terminal
first, only one program can hold the COM port.

Columns: pc_time (local, ISO), then the board's fields:
t_ms, p_mbar, signal_uV, exc_mV, ratio_mV_per_V, die_C, set_mV, ok.
Status lines from the board (anything not starting with DATA) are printed
to the console only.
"""
import argparse
import datetime as dt
import re
import socket
import sys
import time

TCP_PORT = 5000

FIELDS = ["t_ms", "p_mbar", "signal_uV", "exc_mV", "ratio_mV_per_V", "die_C", "set_mV", "ok"]


class LineSock:
    """Minimal line reader over TCP that survives read timeouts."""

    def __init__(self, sock):
        self.sock, self.buf = sock, b""

    def readline(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1024)     # socket.timeout if nothing arrives
            if not chunk:
                line, self.buf = self.buf, b""
                return line                  # b"" = closed
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line + b"\n"

    def write(self, data):
        self.sock.sendall(data)

    def close(self):
        self.sock.close()


class Link:
    """Line-based link to the board: a COM port, or TCP to host[:port]."""

    def __init__(self, target, timeout=2.0):
        self.sock = None
        if re.match(r"^(COM\d+|/dev/)", target, re.I):
            import serial
            self.port = serial.Serial(target, 115200, timeout=timeout)
            self.port.dtr = True    # baud is ignored by USB CDC
        else:
            host, _, port = target.partition(":")
            self.sock = socket.create_connection((host, int(port or TCP_PORT)), timeout=5)
            self.sock.settimeout(timeout)
            self.port = LineSock(self.sock)
        # In case DATA OFF was left set from a terminal session.
        self.write("DATA ON\n")
        # Board clock from the PC's local time.
        self.write(time.strftime("TIME SET %Y-%m-%d %H:%M:%S\n"))

    def readline(self):
        try:
            line = self.port.readline()
        except (socket.timeout, TimeoutError):
            return ""
        if self.sock is not None and line == b"":
            raise ConnectionError("board closed the connection")
        return line.decode("ascii", errors="replace").strip()

    def write(self, text):
        self.port.write(text.encode("ascii"))

    def close(self):
        self.port.close()
        if self.sock is not None:
            self.sock.close()


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
