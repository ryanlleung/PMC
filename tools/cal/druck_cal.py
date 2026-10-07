#!/usr/bin/env python3
"""Show, load or erase the Druck calibration stored on the board.

    python druck_cal.py 192.168.1.23 show
    python druck_cal.py 192.168.1.23 zs --id "cert 1234567" --exc 10 --zero 0.12 --span 99.85
    python druck_cal.py 192.168.1.23 load table.txt --id "OI table" --xunits V --exc 10
    python druck_cal.py 192.168.1.23 erase

The first argument is the board's IP (Ethernet build, TCP port 5000, or
host:port) or its COM port (USB build, needs pip install pyserial).
Stop log_druck.py first: the board takes one connection at a time (a new
one replaces the old), and only one program can hold a COM port.

The board keeps one table of ratio (sensor output / excitation, mV/V)
against pressure (mbar), up to 32 points, linear between points. It is
stored in the on-board serial flash with a CRC and loaded at start-up; if
nothing valid is stored the board uses the nominal 0-10 mV/V = 0-15 psia.

zs     two-point table from a zero/span cert: zero = output at 0 psia, span =
       output at full scale (15 psia) minus zero, both in mV, at --exc volts.
load   table from a text file: any line without two numbers is skipped
       (headers, comments). Columns are sensor output then pressure unless
       --swap. Separators: spaces, tabs, commas or semicolons.

Add --no-save to try a table in RAM only; a power cycle then reverts to
what is in flash. Use --dry to print the commands without a port.
"""
import argparse
import re
import socket
import sys
import time

TCP_PORT = 5000

FS_MBAR = 1034.214           # 15 psia
MAX_POINTS = 32
ID_LEN = 23

P_TO_MBAR = {"mbar": 1.0, "bar": 1000.0, "Pa": 0.01, "kPa": 10.0,
             "psi": 68.947573, "torr": 1.3332237}

NUM = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


def read_table(path, xunits, exc, punits, swap):
    pts = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            fields = [x for x in re.split(r"[\s,;]+", line.strip()) if x]
            if len(fields) < 2 or not all(NUM.fullmatch(x) for x in fields[:2]):
                continue
            x, p = float(fields[0]), float(fields[1])
            if swap:
                x, p = p, x
            if xunits == "V":
                x = x * 1000.0 / exc
            elif xunits == "mV":
                x = x / exc
            pts.append((x, p * P_TO_MBAR[punits]))
    pts.sort()
    return pts


def check(pts):
    if len(pts) < 2:
        sys.exit("need at least 2 points")
    if len(pts) > MAX_POINTS:
        sys.exit(f"{len(pts)} points; the board holds {MAX_POINTS}")
    for a, b in zip(pts, pts[1:]):
        if round(b[0], 6) <= round(a[0], 6):
            sys.exit(f"ratios not strictly ascending at {a[0]:.6f} / {b[0]:.6f} mV/V")


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


class Board:
    def __init__(self, target):
        self.sock = None
        if re.match(r"^(COM\d+|/dev/)", target, re.I):
            import serial
            self.s = serial.Serial(target, 115200, timeout=0.2)
            self.s.dtr = True
            time.sleep(0.2)
            self.s.reset_input_buffer()
        else:
            host, _, port = target.partition(":")
            self.sock = socket.create_connection((host, int(port or TCP_PORT)), timeout=5)
            self.sock.settimeout(0.2)
            self.s = LineSock(self.sock)
            time.sleep(0.2)

    def _readline(self):
        try:
            return self.s.readline().decode("ascii", errors="replace").strip()
        except (socket.timeout, TimeoutError):
            return ""

    def cmd(self, text, timeout=3.0):
        """Sends one line, returns the reply lines up to and including OK/ERR."""
        self.s.write((text + "\n").encode("ascii"))
        got, end = [], time.time() + timeout
        while time.time() < end:
            line = self._readline()
            if not line or line.startswith("DATA,"):
                continue
            if line.startswith("CAL ") or line.startswith("OK") or line.startswith("ERR"):
                got.append(line)
            if line.startswith("OK") or line.startswith("ERR"):
                return got
        sys.exit(f"no reply to '{text}' (old firmware, or wrong address/port?)")


class Dry:
    def cmd(self, text, timeout=0):
        print(text)
        return ["OK"]


def send(board, text):
    reply = board.cmd(text)
    for line in reply:
        print(line)
    if reply[-1].startswith("ERR"):
        sys.exit(1)


def load(board, ident, pts, save):
    check(pts)
    send(board, f"CAL NEW {ident[:ID_LEN]}")
    for x, p in pts:
        send(board, f"CAL PT {x:.6f} {p:.3f}")
    send(board, "CAL APPLY")
    if save:
        send(board, "CAL SAVE")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.splitlines()[1:]))
    ap.add_argument("target", help="board IP or COM port (ignored with --dry)")
    ap.add_argument("--dry", action="store_true", help="print commands, no port")
    sub = ap.add_subparsers(dest="what", required=True)

    sub.add_parser("show")
    sub.add_parser("erase")

    zs = sub.add_parser("zs", help="two-point zero/span")
    zs.add_argument("--id", required=True)
    zs.add_argument("--exc", type=float, required=True, help="cert supply, V")
    zs.add_argument("--zero", type=float, required=True, help="output at 0 psia, mV")
    zs.add_argument("--span", type=float, required=True, help="output at 15 psia minus zero, mV")
    zs.add_argument("--no-save", action="store_true")

    ld = sub.add_parser("load", help="table from a text file")
    ld.add_argument("file")
    ld.add_argument("--id", required=True)
    ld.add_argument("--xunits", choices=["V", "mV", "mVperV"], default="mVperV")
    ld.add_argument("--exc", type=float, help="supply the table was taken at, V (for V or mV)")
    ld.add_argument("--punits", choices=list(P_TO_MBAR), default="mbar")
    ld.add_argument("--swap", action="store_true", help="file has pressure first")
    ld.add_argument("--no-save", action="store_true")

    a = ap.parse_args()
    board = Dry() if a.dry else Board(a.target)

    if a.what == "show":
        send(board, "CAL?")
    elif a.what == "erase":
        send(board, "CAL ERASE")
    elif a.what == "zs":
        z = a.zero / a.exc
        load(board, a.id, [(z, 0.0), (z + a.span / a.exc, FS_MBAR)], not a.no_save)
    elif a.what == "load":
        if a.xunits != "mVperV" and not a.exc:
            sys.exit("--exc is needed when the sensor column is in V or mV")
        pts = read_table(a.file, a.xunits, a.exc, a.punits, a.swap)
        print(f"{len(pts)} points read from {a.file}")
        load(board, a.id, pts, not a.no_save)


if __name__ == "__main__":
    main()
