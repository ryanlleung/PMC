#!/usr/bin/env python3
"""Setpoint step test of the AUTO loop: how the pressure answers a new setpoint.

    python sp_step.py 192.168.1.100 --start 3 --to 5
    python sp_step.py 192.168.1.100 --start 3 --to 5 --back --out cf11_3to5.csv

Puts the board in AUTO at --start, waits until the pressure holds there,
then changes the setpoint to --to and records for --record seconds (with
--back, steps back to --start and records again). Logs one line a second
to a CSV (time, setpoint, pressure, valve, mode, alarms) and prints, for
each step: the time to 90 % of the step, the overshoot, the settling time
(within the band below) and the mean and spread of the last minute.

Standard library only. Needs the Ethernet build with Remote control On;
the board takes one text-port connection at a time, so close the logger,
PuTTY or pmc_gains.py run first. Ctrl+C puts the setpoint back to --start
and leaves AUTO running. The KP and TI are the ones on the board (PID?):
change them on the screen or with PID SET between runs to compare.
"""
import argparse
import csv
import socket
import statistics
import sys
import time

PORT = 8888


class Board:
    def __init__(self, host):
        h, _, p = host.partition(':')
        self.s = socket.create_connection((h, int(p or PORT)), timeout=5)
        self.buf = b''

    def ask(self, line):
        self.s.sendall(line.encode() + b'\n')
        while b'\r\n' not in self.buf:
            d = self.s.recv(1024)
            if not d:
                raise ConnectionError('board closed the connection')
            self.buf += d
        r, self.buf = self.buf.split(b'\r\n', 1)
        return r.decode(errors='replace')


def num(part):
    w = part.split()
    try:
        return float(w[-2] if w[-1] in ('mbar', '%') else w[-1])
    except (ValueError, IndexError):
        return None


def poll(b):
    """Setpoint, pressure, valve, mode, alarms; None where there is no value."""
    r = b.ask('PRES SP?;PRES?;VALV?;MODE?;ALRM?')
    if r.startswith('ERR'):
        raise RuntimeError(r)
    sp, p, v, m, a = r.split(';', 4)
    return num(sp), num(p), num(v), m[5:], a[5:]


def metrics(rows, p0, p1, band):
    """rows: (t from the step, pressure). The step response figures."""
    step = p1 - p0
    if abs(step) < 1e-6 or not rows:
        return {}
    t90 = next((t for t, p in rows if (p - p0) / step >= 0.9), None)
    peak = max((p - p0) / step for _, p in rows)
    settle = 0.0
    for t, p in rows:
        if abs(p - p1) > band:
            settle = t
    last = [p for t, p in rows if t >= rows[-1][0] - 60]
    return {'t90_s': t90, 'overshoot_pct': max(0.0, (peak - 1) * 100),
            'settle_s': settle if settle < rows[-1][0] else None,
            'last_min_mean': statistics.mean(last),
            'last_min_spread': statistics.pstdev(last) if len(last) > 1 else 0.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('host', help='board IP, or IP:port')
    ap.add_argument('--start', type=float, required=True, help='setpoint to settle at first, mbar')
    ap.add_argument('--to', type=float, required=True, help='setpoint to step to, mbar')
    ap.add_argument('--back', action='store_true', help='then step back to --start and record again')
    ap.add_argument('--record', type=float, default=600, help='s to record after each step (default 600)')
    ap.add_argument('--hold', type=float, default=120, help='s the pressure must hold at --start first (default 120)')
    ap.add_argument('--settle-max', type=float, default=1200, help='s to wait for that (default 1200)')
    ap.add_argument('--band', type=float, default=0.1, help='settled within this, mbar (default 0.1)')
    ap.add_argument('--out', default='sp_step.csv', help='CSV to write (default sp_step.csv)')
    ap.add_argument('--speed', type=float, default=1.0, help=argparse.SUPPRESS)   # simulator only
    a = ap.parse_args()
    k = a.speed

    b = Board(a.host)
    print(b.ask('*IDN?'))
    print(b.ask('PID?'))
    r = b.ask('PRES SP %.1f;MODE AUTO;MODE?' % a.start)
    if r != 'MODE AUTO':
        sys.exit('AUTO not started: ' + r)

    f = open(a.out, 'w', newline='')
    w = csv.writer(f)
    w.writerow(['t_s', 'phase', 'sp_mbar', 'p_mbar', 'valve_pct', 'mode', 'alarm'])
    t0 = time.monotonic()
    results = []

    def sample(phase):
        sp, p, v, m, al = poll(b)
        t = (time.monotonic() - t0) * k
        w.writerow([round(t, 1), phase, sp, p, v, m, al])
        if m != 'AUTO':
            raise RuntimeError('left AUTO (%s, alarms %s)' % (m, al))
        time.sleep(1.0 / k)
        return t, p

    try:
        # Settle at --start: the pressure within the band for --hold s.
        print('settling at %.1f mbar' % a.start)
        since = None
        while True:
            t, p = sample('settle')
            if p is not None and abs(p - a.start) <= a.band:
                since = since if since is not None else t
                if t - since >= a.hold:
                    break
            else:
                since = None
            if t > a.settle_max:
                sys.exit('did not hold %.1f mbar within %d s' % (a.start, a.settle_max))
        steps = [(a.start, a.to)] + ([(a.to, a.start)] if a.back else [])
        for p0, p1 in steps:
            b.ask('PRES SP %.1f;PRES SP?' % p1)
            print('step %.1f -> %.1f mbar, recording %d s' % (p0, p1, a.record))
            ts, rows = None, []
            while True:
                t, p = sample('%.1f->%.1f' % (p0, p1))
                ts = t if ts is None else ts
                if p is not None:
                    rows.append((t - ts, p))
                if t - ts >= a.record:
                    break
            results.append((p0, p1, metrics(rows, p0, p1, a.band)))
    except KeyboardInterrupt:
        print('stopped')
    finally:
        try:
            b.ask('PRES SP %.1f;PRES SP?' % a.start)
        except OSError:
            pass
        f.close()

    print('written', a.out)
    fmt = lambda v, u: '-' if v is None else '%.0f %s' % (v, u)
    for p0, p1, m in results:
        if not m:
            continue
        print('%.1f -> %.1f mbar: 90 %% in %s, overshoot %.0f %%, settled (+-%.2f) in %s, '
              'last minute %.3f mbar +- %.3f' % (p0, p1, fmt(m['t90_s'], 's'), m['overshoot_pct'], a.band,
                                                 fmt(m['settle_s'], 's'), m['last_min_mean'], m['last_min_spread']))


if __name__ == '__main__':
    main()
