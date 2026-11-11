#!/usr/bin/env python3
"""Learned fit of the bench cryostat's pressure response, checked on runs it
has not seen.

    python3 fit.py <folder of pmc_session_*.csv> [--out results]

The learned model is the physics model (bench.c of 10 Nov, bench_10nov.py) times
a learned correction: gradient-boosted trees that read the valve's recent
history and the physics model's own state, and give the factor the bench
pressure is off the physics by. No correction is the physics model itself.

Every group of runs is left out in turn: the correction is trained on the
others and scored on the one left out, against the physics model on the same
samples. Three sets of inputs are tried:
  valve              the valve alone (what a simulator has)
  valve + cold head  and the logged cold head temperature (Lake Shore D)
  valve + both       and the logged heat exchanger temperature (Lake Shore A);
                     the physics model is then given it too
Each run gets one number first, for both models alike: its valve offset, the
shift of the valve scale that makes the physics model fit that run best
(the valve's zero moves from one homing to the next). Only samples with the
heat exchanger under 20 K are trained on and scored (the cryostat cold), and
the first 10 min of a run are not scored.

Needs numpy, scikit-learn and matplotlib.
"""
import argparse
import os
import pickle
from multiprocessing import Pool

import numpy as np
from sklearn.ensemble import HistGradientBoostingRegressor

from logs import load_runs
from physics import CAL, W, features, offsets, phys, sensor_features

BANDS = [(0, 2, 'under 2'), (2, 4, '2 to 4'), (4, 8, '4 to 8'), (8, 24, '8 to 24'), (24, 1e9, 'over 24')]
VARIANTS = [('valve', 'X', 'pm'), ('valve + cold head', 'Xd', 'pm'), ('valve + both', 'Xda', 'pm_log')]
GBM = dict(max_depth=4, learning_rate=0.05, max_iter=200, min_samples_leaf=300, l2_regularization=1.0,
           loss='absolute_error', random_state=0)   # set before the first held-out score and not tuned on them


def rmse(p, m):
    return float(np.sqrt(np.mean((m - p) ** 2)))


def rel(p, m):
    return float(np.median(np.abs(m - p) / p) * 100)


def prepare(runs, cache):
    """Offsets, physics runs and features for every run (cached: slow)."""
    if cache and os.path.exists(cache):
        with open(cache, 'rb') as f:
            done = pickle.load(f)
        if [(r['name'], len(r['t'])) for r in runs] == [(r['name'], len(r['t'])) for r in done]:
            return done
    with Pool() as pool:
        for r in runs:
            r['p'] = np.maximum(r['p'], 0.05)
            r['D'] = np.where(np.isnan(r['D']), 4.0, r['D'])
            r['dv_cal'], r['dv'] = offsets(r, pool)
            v = r['v'] + r['dv']
            r['pm0'], _ = phys(r['v'], r['A'], r['p'])
            r['pm'], r['am'] = phys(v, r['A'], r['p'])
            r['pm_log'], _ = phys(v, r['A'], r['p'], own_hx=False)
            f = features(v, r['pm'], r['am'])
            fd = dict(f, **sensor_features(r['D']))
            fda = dict(f, **sensor_features(r['D'], r['A'], r['am']))
            fda['phys_log_p'] = np.log(r['pm_log'])
            r['names'] = list(fda)
            r['X'], r['Xd'], r['Xda'] = (np.stack(list(x.values()), -1) for x in (f, fd, fda))
            print('  %-12s valve offset %+.2f %% (from its first 10 min alone: %+.2f %%)' % (r['name'], r['dv'], r['dv_cal']), flush=True)
    if cache:
        with open(cache, 'wb') as f:
            pickle.dump(runs, f)
    return runs


def weights(p):
    """Each pressure band counts about the same, however long the logs sat in it."""
    w = np.ones(len(p))
    for lo, hi, _ in BANDS:
        k = (p >= lo) & (p < hi)
        if k.sum():
            w[k] = 1 / np.sqrt(k.sum())
    return w / w.mean()


def train(runs, xkey, base):
    X = np.concatenate([r[xkey][W:] for r in runs])
    p = np.concatenate([r['p'][W:] for r in runs])
    b = np.concatenate([r[base][W:] for r in runs])
    k = np.concatenate([r['A'][W:] for r in runs]) < 20
    return HistGradientBoostingRegressor(**GBM).fit(X[k], (np.log(p) - np.log(b))[k], sample_weight=weights(p[k]))


def scored(r):
    return (np.arange(len(r['p'])) >= CAL) & (r['A'] < 20)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder')
    ap.add_argument('--out', default='results')
    ap.add_argument('--no-plots', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    runs, recs = load_runs(a.folder)
    lines = []

    def say(s=''):
        print(s, flush=True)
        lines.append(s)
    say('%d files, %d samples, %d after removing the ones several saves share; %d runs in %d groups' % (
        sum(len(r['files']) for r in recs), sum(r['n_raw'] for r in recs), sum(len(r['t']) for r in recs),
        len(runs), len({r['group'] for r in runs})))
    say('Valve offset of each run (fitted with the physics model):')
    runs = prepare(runs, os.path.join(a.out, 'features.pkl'))
    for r in runs:
        lines.append('  %-12s %+.2f %%  (from its first 10 min alone: %+.2f %%)' % (r['name'], r['dv'], r['dv_cal']))
    groups = sorted({r['group'] for r in runs})
    for label, xkey, base in VARIANTS:
        for g in groups:
            model = train([r for r in runs if r['group'] != g], xkey, base)
            for r in runs:
                if r['group'] == g:
                    r['ml ' + label] = r[base] * np.exp(model.predict(r[xkey]))
    say()
    say('Held-out runs, pressure error (rms mbar / median %), heat exchanger under 20 K, after the first 10 min')
    say('%-13s %6s | %-13s | %-13s %-13s | %-13s %-13s | %-13s %-13s' % (
        'run', 's', 'no offset', 'physics', 'learned', 'physics', 'learned', 'physics', 'learned'))
    say('%-13s %6s | %-13s | %-27s | %-27s | %-27s' % ('', '', 'physics', 'valve', 'valve + cold head', 'valve + both (logged HX)'))

    def cell(p, m):
        return '%5.2f %4.0f %%  ' % (rmse(p, m), rel(p, m))
    tot = []
    for r in runs:
        k = scored(r)
        if k.sum() < 300:
            continue
        p = r['p'][k]
        say('%-13s %6d | %s| %s%s| %s%s| %s%s' % (r['name'], k.sum(), cell(p, r['pm0'][k]), cell(p, r['pm'][k]), cell(p, r['ml valve'][k]),
                                                 cell(p, r['pm'][k]), cell(p, r['ml valve + cold head'][k]),
                                                 cell(p, r['pm_log'][k]), cell(p, r['ml valve + both'][k])))
        tot.append({'p': p, 'pm0': r['pm0'][k], 'pm': r['pm'][k], 'pm_log': r['pm_log'][k],
                    **{key: r[key][k] for key in r if key.startswith('ml ')}})
    T = {key: np.concatenate([t[key] for t in tot]) for key in tot[0]}

    def row(name, k):
        p = T['p'][k]
        say('%-13s %6d | %s| %s%s| %s%s| %s%s' % (name, k.sum(), cell(p, T['pm0'][k]), cell(p, T['pm'][k]), cell(p, T['ml valve'][k]),
                                                 cell(p, T['pm'][k]), cell(p, T['ml valve + cold head'][k]),
                                                 cell(p, T['pm_log'][k]), cell(p, T['ml valve + both'][k])))
    say('By bench pressure, mbar:')
    for lo, hi, name in BANDS:
        k = (T['p'] >= lo) & (T['p'] < hi)
        if k.sum():
            row(name, k)
    row('all', np.ones(len(T['p']), bool))
    with open(os.path.join(a.out, 'results.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    if not a.no_plots:
        import plots
        plots.overlays(runs, a.out, scored)
        plots.summary(T, BANDS, a.out)


if __name__ == '__main__':
    main()
