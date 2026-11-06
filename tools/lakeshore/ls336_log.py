#!/usr/bin/env python3
"""Log Lake Shore Model 336 temperatures over Ethernet to a CSV.

    python ls336_log.py 192.168.1.165                  # A heat exchanger, D cold head, every 1 s
    python ls336_log.py 192.168.1.165 --inputs A "B=Sample" D --every 5 --out run1.csv

Asks KRDG? (kelvin) for each input on TCP port 7777, the 336's Ethernet
command port, and writes one row per reading: the PC's date and time, the
seconds from the start, and one column per input in K, named after the
input and what it measures (A_heat_exchanger_K, D_cold_head_K). It prints each row
as well, and keeps going if the link drops (it reconnects every 5 s and
leaves those rows out). Ctrl+C stops it.

Standard library only (Python 3.8 or later). The 336 needs its Ethernet
on (Interface menu, DHCP or a static address on the same subnet as this
PC). A reading the 336 cannot give (sensor open, over range) comes back
as 0 or with an overrange flag; the column is left empty for those.
"""
import argparse
import csv
import datetime
import re
import socket
import sys
import time

PORT = 7777
# The 336 inputs on this rig, used when an input is given without a name.
NAMES = {'A': 'Heat exchanger', 'D': 'Cold head'}


class LS336:
    def __init__(self, host):
        h, _, p = host.partition(':')
        self.addr = (h, int(p or PORT))
        self.s = None
        self.buf = b''

    def connect(self):
        self.s = socket.create_connection(self.addr, timeout=5)
        self.buf = b''

    def close(self):
        if self.s:
            self.s.close()
        self.s = None

    def ask(self, line):
        if self.s is None:
            self.connect()
        self.s.sendall(line.encode() + b'\r\n')
        while b'\n' not in self.buf:
            d = self.s.recv(1024)
            if not d:
                raise ConnectionError('the 336 closed the connection')
            self.buf += d
        r, self.buf = self.buf.split(b'\n', 1)
        return r.decode(errors='replace').strip()


def kelvin(text):
    """'+1.6234E+00' or '+001.623' to a float; None for no reading."""
    try:
        v = float(text)
    except ValueError:
        return None
    return v if v > 0 else None


def column(spec):
    """'A=Heat exchanger' or 'A' to ('A', 'Heat exchanger', 'A_heat_exchanger_K')."""
    i, _, name = spec.partition('=')
    i = i.strip().upper()
    if not re.match(r'^[A-D][1-5]?$', i):
        sys.exit('336 inputs are A to D (D1 to D5 with the 3062 card), not "%s"' % i)
    name = name.strip() or NAMES.get(i, 'Input ' + i)
    slug = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')
    return i, name, '%s_%s_K' % (i, slug) if slug else '%s_K' % i


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('host', help="the 336's IP address (port 7777 unless given as IP:port)")
    ap.add_argument('--inputs', nargs='+', default=['A', 'D'], metavar='IN[=NAME]',
                    help='inputs to read, each with what it measures (default A D: A heat exchanger, D cold head)')
    ap.add_argument('--every', type=float, default=1.0, help='seconds between readings (default 1)')
    ap.add_argument('--out', default=None, help='CSV file (default ls336_<date>_<time>.csv)')
    a = ap.parse_args()
    cols = [column(x) for x in a.inputs]
    inputs = [c[0] for c in cols]
    out = a.out or datetime.datetime.now().strftime('ls336_%Y%m%d_%H%M%S.csv')

    ls = LS336(a.host)
    try:
        print(ls.ask('*IDN?'))
    except OSError as e:
        sys.exit('no answer from %s:%d (%s)' % (ls.addr + (e,)))

    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['time', 't_s'] + [c[2] for c in cols])
        print('logging %s every %g s to %s  (Ctrl+C to stop)' % (
            ', '.join('%s (%s)' % (c[1], c[0]) for c in cols), a.every, out))
        t0 = nxt = time.monotonic()
        try:
            while True:
                try:
                    vals = [kelvin(ls.ask('KRDG? ' + i)) for i in inputs]
                except OSError as e:
                    print('link lost (%s), trying again in 5 s' % e)
                    ls.close()
                    time.sleep(5)
                    nxt = time.monotonic()
                    continue
                now = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                row = [now, round(time.monotonic() - t0, 1)] + ['' if v is None else '%.4f' % v for v in vals]
                w.writerow(row)
                f.flush()
                print('  '.join(str(x) for x in row))
                nxt += a.every
                time.sleep(max(0.0, nxt - time.monotonic()))
        except KeyboardInterrupt:
            print('stopped; written', out)
        finally:
            ls.close()


if __name__ == '__main__':
    main()
