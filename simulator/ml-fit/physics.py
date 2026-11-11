"""The physics model (bench.c as of 10 Nov 2026, kept in bench_10nov.py) on a logged valve, the
valve offset of a run, and the valve-history features the learned correction
reads."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_10nov as fb  # noqa: E402

W = 120     # s at the start of a run where the model is given the logged heat exchanger temperature
CAL = 600   # s at the start of a run that are never scored
DVS = np.round(np.arange(-0.8, 0.8001, 0.05), 2)


def phys(v, A, p, own_hx=True):
    """Pressure and heat exchanger temperature from the valve: W s on the
    logged temperature, then on the model's own (or the logged one all the way)."""
    m0, _ = fb.model(v[:W], A[:W], p[0])
    m, a = fb.model(v[W - 1:], A[W - 1:], m0[-1], own_hx=own_hx)
    return np.concatenate([m0[:-1], m]), np.concatenate([A[:W - 1], a])


def _score(args):
    v, A, p, dv, n = args
    m, _ = phys(v[:n] + dv, A[:n], p[:n])
    k = A[W:n] < 20
    if k.sum() < 50:
        k = np.ones(n - W, bool)
    return np.mean((np.log(m[W:]) - np.log(p[W:n]))[k] ** 2) + 1e-3 * abs(dv)


def offsets(run, pool):
    """Where the valve's zero sat in this run, as the % to add to the logged
    valve that makes the physics model fit best: from the first CAL s alone,
    and from the whole run."""
    v, A, p = run['v'], run['A'], np.maximum(run['p'], 0.05)
    e_cal = pool.map(_score, [(v, A, p, dv, CAL) for dv in DVS])
    e_all = pool.map(_score, [(v, A, p, dv, len(v)) for dv in DVS])
    return float(DVS[int(np.argmin(e_cal))]), float(DVS[int(np.argmin(e_all))])


def ema(x, tau):
    out = np.empty(len(x))
    a, s = 1 - np.exp(-1 / tau), x[0]
    for i, xi in enumerate(x):
        s += (xi - s) * a
        out[i] = s
    return out


def features(v, pm, am):
    """What the correction sees each second. All of it comes from the valve
    (v, offset added) and the physics model run on it (pm, am)."""
    n, f = len(v), {}
    y, yy = np.empty(n), v[0]
    for i in range(n):
        if v[i] < yy or v[i] - yy > fb.stick(yy):
            yy = v[i]
        y[i] = yy
    f['valve'], f['needle'], f['stuck_gap'] = v, y, v - y
    for tau in (5, 20, 60, 200, 600, 2000):
        f['valve_minus_avg_%ds' % tau] = v - ema(v, tau)
    dv = np.diff(v, prepend=v[0])
    t_open, t_close, last = np.empty(n), np.empty(n), np.empty(n)
    to = tc = 3600.0
    lm = 0.0
    for i in range(n):
        to += 1
        tc += 1
        if dv[i] > 0.005:
            to, lm = 0, (dv[i] if lm <= 0 else lm + dv[i])
        elif dv[i] < -0.005:
            tc, lm = 0, (dv[i] if lm >= 0 else lm + dv[i])
        t_open[i], t_close[i], last[i] = to, tc, lm
    f['log_s_since_opening'] = np.log1p(np.minimum(t_open, 3600))
    f['log_s_since_closing'] = np.log1p(np.minimum(t_close, 3600))
    f['last_move_pct'] = last
    f['travel_60s'], f['travel_600s'] = ema(np.abs(dv), 60) * 60, ema(np.abs(dv), 600) * 600
    lp = np.log(pm)
    f['phys_log_p'], f['phys_hx'] = lp, am
    f['phys_log_target'] = np.log(fb.FLOOR + np.interp(y, fb.F_Y, fb.F_P) * np.exp(-(am - 4) / fb.KA))
    for tau in (60, 600, 2000):
        f['phys_log_p_minus_avg_%ds' % tau] = lp - ema(lp, tau)
    f['phys_hx_minus_avg_600s'] = am - ema(am, 600)
    return f


def sensor_features(D, A=None, am=None):
    """The logged temperatures, for the variants that are given them."""
    f = {'cold_head_K': D, 'cold_head_minus_avg_600s': D - ema(D, 600), 'cold_head_avg_600s': ema(D, 600)}
    if A is not None:
        f.update({'hx_K': A, 'hx_minus_avg_60s': A - ema(A, 60), 'hx_minus_avg_600s': A - ema(A, 600), 'hx_minus_phys': A - am})
    return f
