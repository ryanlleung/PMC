#!/usr/bin/env python3
"""Checks the simulator's bench cryostat (bench.c, PLANT_BENCH) against bench logs.

    python3 fit_bench.py [--valve PCT] [--cold K] session.csv [session.csv ...]

Each CSV is a control test GUI session or autotune samples file (t_s,
sp_mbar, p_mbar, valve_pct, mode, alarm, stage, A_heat_exchanger_K,
D_cold_head_K, ...): 1 s samples of the valve, the pressure and the Lake
Shore's heat exchanger (channel A) and cold head (channel D) temperatures.
Needs numpy only.

The model is bench.c's, with the same numbers (keep them the same):
  needle    follows a closing valve at once and an opening valve PLAY % of
            travel behind
  target    FLOOR + F(needle) x G(Thx): G 1.33 at 1.8 K, 0.63 from 8 K
  pressure  the target after DEAD s, through a lag LAG(Thx): 25 to 70 s with
            liquid in the heat exchanger (Thx under about 3.5 K), 5 s dry,
            and never more than MOST(pressure): 2 s at 2 mbar, 8 s at 6 mbar
  cold head towards COLD(pressure), over CTAU(cold head) s cooling and CUP
            times that warming
  Thx       towards WET(pressure) x share + DRY(pressure) x (1 - share), the
            share of liquid from SHARE(cold head): all under 3.72 K, none
            over 4.03 K. It moves at most HXSTEP K's worth at a time, over
            HXTAU x (Thx / 2.8 K) ^ HXPOW s

Two things move from one bench session to the next. The numbers here are
for the 11 Nov 2026 17:26 step test session; for another session give
  --valve   what to add to the logged valve, % (9 Nov: -0.6, 10 Nov: -0.2,
            11 Nov morning: -0.3)
  --cold    what to add to the cold head's steady temperature, K (9 and
            10 Nov: -0.2)

For each stage of each log this prints the bench pressure and the model's,
run from the logged valve with the logged Thx and with the model's own Thx
and cold head; for each relay test, the first-harmonic swing and phase
(pressure against valve), and the lag that would match that relay alone.

Where the numbers came from: LAG, MOST, FLOOR and DEAD from the 9 and
10 Nov logs (relays, and a reach from 10 down to 1 mbar with KP 1.20 that
swung 1 to 10 mbar). Everything else was refitted on 11 Nov to the session
logs of 9 to 11 Nov (ten runs, 85,000 s), with the 11 Nov step test
(8 to 48 mbar, down to 2 and back to 8) weighted three times: first the
cold head from the logged pressure, then Thx from the logged pressure and
cold head, then the pressure from the logged temperatures, then F, G and
PLAY again with the model running from the valve alone.
"""
import csv
import math
import sys

import numpy as np

F_Y = np.array([0, 1.68, 1.88, 2.28, 2.48, 2.73, 2.98, 3.88, 6.28, 7.28, 8.68, 9.98, 12.78, 19.28, 24.3, 31.28, 49.28, 100])
F_P = np.array([0, 0, 0.4, 0.88, 4.76, 7.4, 9, 12.1, 18.8, 21.8, 26.8, 31.1, 40.1, 52.7, 69.2, 75, 79.9, 87.2])
G_T = np.array([1.8, 2.5, 3.5, 5, 6.5, 8, 12])
G_F = np.array([1.33, 1.09, 1, 0.92, 0.76, 0.63, 0.63])
LAG_T = np.array([1.8, 2.0, 2.75, 3.5, 4.5, 5.7, 7, 12, 30])
LAG_S = np.array([70, 64, 26, 24, 12, 5, 5, 5, 5.0])
MOST_P = np.array([1, 2, 4, 6, 8, 12])
MOST_S = np.array([1.5, 2, 4, 8, 30, 1000.0])
COLD_P = np.array([1, 2, 4, 8, 10, 12, 14.5, 18, 24, 29, 48])
COLD_T = np.array([2.91, 3.02, 3.09, 3.46, 3.56, 3.88, 4.09, 4.21, 4.93, 5.92, 10.88])
CTAU_T = np.array([3.5, 4, 5, 6, 30])
CTAU_S = np.array([255, 100, 100, 150, 150.0])
SHARE_C = np.array([3.72, 3.78, 3.88, 3.98, 4.03, 30])
SHARE_X = np.array([1, 0.85, 0.66, 0.13, 0, 0])
WET_P = np.array([1, 2, 4, 6, 8, 9, 10, 11, 12, 12.5, 60])
WET_T = np.array([19, 8, 7.13, 4.77, 3.47, 2.92, 2.61, 2.06, 2.05, 1.81, 1.81])
DRY_P = np.array([4, 8, 12, 16, 20, 24, 29, 46, 55])
DRY_T = np.array([9, 8, 6.8, 6.2, 5.7, 5.9, 7, 9.8, 13.9])
FLOOR, DEAD, PLAY, CUP, HXTAU, HXPOW, HXMIN, HXSTEP = 0.67, 2, 0.3, 1.5, 12.5, 2.64, 4.0, 3.0


def table(xs, ys, x):
    """bench.c's table(): flat below the first point, on as the last piece above the last."""
    if x >= xs[-1]:
        return ys[-1] + (ys[-1] - ys[-2]) * (x - xs[-1]) / (xs[-1] - xs[-2])
    return float(np.interp(x, xs, ys))


def needle(y, x):
    """The needle after the valve moved to x: closing follows at once, opening PLAY behind."""
    return x if x < y else max(y, x - PLAY)


def target(y, a):
    return FLOOR + table(F_Y, F_P, y) * table(G_T, G_F, a)


def model(x, hx, p0, lag=None, own_hx=False, cold=None, cold_offset=0.0):
    """Pressure, Thx and cold head from the logged valve, 1 s steps. lag: a
    fixed lag in s instead of LAG(Thx); own_hx: Thx and the cold head from
    the model (starting at hx[0] and cold, or the steady cold head for p0),
    not the log."""
    y, p, a = x[0], p0, hx[0]
    c = cold if cold is not None else table(COLD_P, COLD_T, p0) + cold_offset
    buf = [None] * (DEAD + 1)
    out, out_hx, out_c = np.empty(len(x)), np.empty(len(x)), np.empty(len(x))
    for i in range(len(x)):
        y = needle(y, x[i])
        if not own_hx:
            a = hx[i]
        buf.pop()
        buf.insert(0, target(y, a))
        delayed = buf[DEAD] if buf[DEAD] is not None else buf[0]
        tau = lag or min(table(LAG_T, LAG_S, a), table(MOST_P, MOST_S, p))
        p += (delayed - p) * (1 - math.exp(-1 / tau))
        if own_hx:
            ce = table(COLD_P, COLD_T, p) + cold_offset
            tc = table(CTAU_T, CTAU_S, c) * (CUP if ce > c else 1)
            c += (ce - c) * (1 - math.exp(-1 / tc))
            share = table(SHARE_C, SHARE_X, c)
            T = share * table(WET_P, WET_T, p) + (1 - share) * table(DRY_P, DRY_T, p)
            th = max(HXMIN, HXTAU * (a / 2.8) ** HXPOW)
            a += min(HXSTEP, max(-HXSTEP, T - a)) * (1 - math.exp(-1 / th))
        out[i], out_hx[i], out_c[i] = p, a, c
    return out, out_hx, out_c


def first_harmonic(t, p, x):
    """Swing (mbar, first harmonic) and phase of the pressure against the
    valve over the last two whole cycles of a relay; None if too few."""
    mid = (x.max() + x.min()) / 2
    up = np.where((x[1:] > mid) & (x[:-1] <= mid))[0] + 1
    if len(up) < 3:
        return None
    a, b = up[-3], up[-1]
    w = 2 * math.pi / ((t[b] - t[a]) / 2)
    e = np.exp(-1j * w * t[a:b])
    cx, cp = np.sum((x[a:b] - x[a:b].mean()) * e), np.sum((p[a:b] - p[a:b].mean()) * e)
    ph = (np.angle(cp) - np.angle(cx) + math.pi) % (2 * math.pi) - math.pi
    return 2 * abs(cp) / (b - a), math.degrees(ph), (t[b] - t[a]) / 2


def fill(a):
    bad = np.isnan(a)
    if bad.all():
        return None
    a[bad] = np.interp(np.flatnonzero(bad), np.flatnonzero(~bad), a[~bad])
    return a


def load(path):
    t, p, x, hx, cold, st = [], [], [], [], [], []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                row = float(r['t_s']), float(r['p_mbar']), float(r['valve_pct'])
            except (KeyError, ValueError):
                continue
            t.append(row[0]), p.append(row[1]), x.append(row[2]), st.append(r.get('stage') or '')
            for col, name in ((hx, 'A_heat_exchanger_K'), (cold, 'D_cold_head_K')):
                try:
                    col.append(float(r.get(name) or 'nan'))
                except ValueError:
                    col.append(float('nan'))
    hx = fill(np.array(hx))
    if hx is None:
        raise SystemExit('%s: no heat exchanger temperature (A_heat_exchanger_K)' % path)
    return np.array(t) - t[0], np.array(p), np.array(x), hx, fill(np.array(cold)), st


def main():
    args, dv, dc = sys.argv[1:], 0.0, 0.0
    while args and args[0] in ('--valve', '--cold'):
        if args[0] == '--valve':
            dv = float(args[1])
        else:
            dc = float(args[1])
        args = args[2:]
    if not args:
        raise SystemExit(__doc__)
    for path in args:
        t, p, x, hx, cold, st = load(path)
        x = x + dv
        m, _, _ = model(x, hx, p[0])
        mo, mo_hx, mo_c = model(x, hx, p[0], own_hx=True, cold=None if cold is None else cold[0], cold_offset=dc)
        print('== %s: rms %.2f mbar (logged Thx), %.2f mbar (model Thx, rms %.2f K off%s)' % (
            path, np.sqrt(np.mean((m - p) ** 2)), np.sqrt(np.mean((mo - p) ** 2)), np.sqrt(np.mean((mo_hx - hx) ** 2)),
            '' if cold is None else '; cold head rms %.2f K off' % np.sqrt(np.mean((mo_c - cold) ** 2))))
        i = 0
        while i < len(st):
            j = i
            while j < len(st) and st[j] == st[i]:
                j += 1
            name = st[i] or '-'
            line = '%-22s %5d s  P bench %6.2f model %6.2f (own Thx %6.2f)  Thx %5.2f K (model %5.2f)' % (
                name, j - i, p[i:j].mean(), m[i:j].mean(), mo[i:j].mean(), hx[i:j].mean(), mo_hx[i:j].mean())
            if name.endswith('relay'):
                b, s = first_harmonic(t[i:j], p[i:j], x[i:j]), first_harmonic(t[i:j], m[i:j], x[i:j])
                if b and s:
                    best = min((abs(math.log(r[0] / b[0])) + abs(r[1] - b[1]) / 30, lag) for lag in
                               (1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64)
                               for r in [first_harmonic(t[i:j], model(x[i:j], hx[i:j], p[i], lag=lag)[0], x[i:j])] if r)
                    line += '\n%24s swing %.2f / %.2f mbar, phase %.0f / %.0f deg, period %.1f s; lag alone %d s' % (
                        'bench / model:', b[0], s[0], b[1], s[1], b[2], best[1])
            print(line)
            i = j


if __name__ == '__main__':
    main()
