#!/usr/bin/env python3
"""Checks the simulator's bench cryostat (bench.c, PLANT_BENCH) against bench logs.

    python3 fit_bench.py session.csv [session.csv ...]

Each CSV is a control test GUI session or autotune samples file (t_s,
sp_mbar, p_mbar, valve_pct, mode, alarm, stage, A_heat_exchanger_K, ...):
1 s samples of the valve, the pressure and the Lake Shore's heat exchanger
temperature (channel A). Needs numpy only.

The model is bench.c's, with the same numbers (keep them the same):
  needle   follows a closing valve at once; an opening move sticks until it
           is STICK % ahead (0.37 % up to 5 % open, 0.05 % from 10 %), then
           the needle slips to it
  target   FLOOR + F(needle) x exp(-(Thx - 4 K) / KA)
  pressure the target after DEAD s, through a lag LAG(Thx): 25 to 70 s with
           liquid in the heat exchanger (Thx under about 3.5 K), 5 s dry
  Thx      towards HXSS(pressure) over HXTAU s

For each stage of each log this prints the bench pressure and the model's,
run from the logged valve with the logged Thx and with the model's own Thx;
for each relay test, the first-harmonic swing and phase (pressure against
valve), and the lag that would match that relay alone. The 9 Nov 2026
numbers came from these comparisons on the 16:23 session and that day's two
autotune runs (1 to 24 mbar): F and HXSS from the held setpoints, LAG from
the relays (the per-relay lags against Thx), STICK from the 6 Nov steps.
"""
import csv
import math
import sys

import numpy as np

F_Y = np.array([0, 2.4, 2.6, 3.0, 3.2, 3.45, 3.7, 4.6, 7.0, 8.0, 9.4, 10.7, 13.5, 20, 32, 50])
F_P = np.array([0, 0, 0.9, 2.5, 5.5, 7.0, 8.6, 12.3, 18.0, 20.5, 25.3, 27.7, 33.6, 48.5, 76, 98])
LAG_T = np.array([1.8, 2.0, 2.75, 3.5, 4.5, 5.7, 7, 12, 30])
LAG_S = np.array([70, 64, 26, 24, 12, 5, 5, 5, 5.0])
HX_P = np.array([0.67, 1, 2, 4, 8, 12, 16, 20, 28, 50])
HX_T = np.array([20, 16, 14, 7, 3.2, 1.9, 4.5, 5.6, 6.5, 8.8])
FLOOR, KA, DEAD, HXTAU = 0.67, 10.0, 2, 340.0


def stick(y):
    f = min(1.0, max(0.0, (y - 5.0) / 5.0))
    return 0.37 * (1 - f) + 0.05 * f


def model(x, hx, p0, lag=None, own_hx=False):
    """Pressure (and Thx) from the logged valve, 1 s steps. lag: a fixed lag
    in s instead of LAG(Thx); own_hx: Thx from the model, not the log."""
    y, p, a = x[0], p0, hx[0]
    buf = [None] * (DEAD + 1)
    out, out_hx = np.empty(len(x)), np.empty(len(x))
    for i in range(len(x)):
        if x[i] < y:
            y = x[i]
        elif x[i] - y > stick(y):
            y = x[i]
        if not own_hx:
            a = hx[i]
        target = FLOOR + np.interp(y, F_Y, F_P) * math.exp(-(a - 4.0) / KA)
        buf.pop()
        buf.insert(0, target)
        delayed = buf[DEAD] if buf[DEAD] is not None else target
        tau = lag or float(np.interp(a, LAG_T, LAG_S))
        p += (delayed - p) * (1 - math.exp(-1 / tau))
        if own_hx:
            a += (np.interp(p, HX_P, HX_T) - a) * (1 - math.exp(-1 / HXTAU))
        out[i], out_hx[i] = p, a
    return out, out_hx


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


def load(path):
    t, p, x, hx, st = [], [], [], [], []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                row = float(r['t_s']), float(r['p_mbar']), float(r['valve_pct'])
            except (KeyError, ValueError):
                continue
            t.append(row[0]), p.append(row[1]), x.append(row[2]), st.append(r.get('stage') or '')
            try:
                hx.append(float(r.get('A_heat_exchanger_K') or 'nan'))
            except ValueError:
                hx.append(float('nan'))
    hx = np.array(hx)
    bad = np.isnan(hx)
    if bad.all():
        raise SystemExit('%s: no heat exchanger temperature (A_heat_exchanger_K)' % path)
    hx[bad] = np.interp(np.flatnonzero(bad), np.flatnonzero(~bad), hx[~bad])
    return np.array(t) - t[0], np.array(p), np.array(x), hx, st


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for path in sys.argv[1:]:
        t, p, x, hx, st = load(path)
        m, _ = model(x, hx, p[0])
        mo, mo_hx = model(x, hx, p[0], own_hx=True)
        print('== %s: rms %.2f mbar (logged Thx), %.2f mbar (model Thx, rms %.2f K off)' % (
            path, np.sqrt(np.mean((m - p) ** 2)), np.sqrt(np.mean((mo - p) ** 2)), np.sqrt(np.mean((mo_hx - hx) ** 2))))
        i = 0
        while i < len(st):
            j = i
            while j < len(st) and st[j] == st[i]:
                j += 1
            name = st[i] or '-'
            line = '%-22s %5d s  P bench %6.2f model %6.2f (own Thx %6.2f)  Thx %5.2f K' % (
                name, j - i, p[i:j].mean(), m[i:j].mean(), mo[i:j].mean(), hx[i:j].mean())
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
