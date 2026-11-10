"""Bench logs for the learned fit: read the control test GUI's CSVs, drop the
samples that several saves share, and cut what is left into runs.

The GUI saves its last 4 h, so saves a few minutes apart hold mostly the same
samples. Files that share samples (same t_s, pressure and valve) are one
recording. A run is a stretch of a recording with no gap over 5 s, in MANUAL
or AUTO, with a valve position, put on a 1 s grid. Runs of a recording less
than an hour apart form one group; the fit leaves out one group at a time.
"""
import csv
import datetime
import glob
import os
import re

import numpy as np


def read(path):
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))

    def col(k):
        out = np.full(len(rows), np.nan)
        for i, r in enumerate(rows):
            try:
                out[i] = float(r.get(k) or 'nan')
            except ValueError:
                pass
        return out
    d = dict(t=col('t_s'), sp=col('sp_mbar'), p=col('p_mbar'), v=col('valve_pct'),
             A=col('A_heat_exchanger_K'), D=col('D_cold_head_K'))
    for k in ('mode', 'alarm', 'stage'):
        d[k] = np.array([r.get(k) or '' for r in rows])
    d['file'] = os.path.basename(path)
    m = re.search(r'(\d{8})_(\d{6})', d['file'])
    d['saved'] = datetime.datetime.strptime(m.group(1) + m.group(2), '%Y%m%d%H%M%S') if m else None
    return d


def recordings(paths):
    files = [read(p) for p in sorted(paths)]
    keys = [set(zip(np.round(d['t'], 2), d['p'], np.nan_to_num(d['v'], nan=-1))) for d in files]
    group = list(range(len(files)))
    for i in range(len(files)):
        for j in range(i):
            if len(keys[i] & keys[j]) > 20:
                gi, gj = group[i], group[j]
                group = [gj if g == gi else g for g in group]
    recs = []
    for g in sorted(set(group)):
        idx = [i for i in range(len(files)) if group[i] == g]
        cat = {k: np.concatenate([files[i][k] for i in idx]) for k in ('t', 'sp', 'p', 'v', 'A', 'D', 'mode', 'alarm', 'stage')}
        _, first = np.unique(np.round(cat['t'], 2), return_index=True)
        rec = {k: cat[k][first] for k in cat}
        rec['files'] = [files[i]['file'] for i in idx]
        rec['n_raw'] = sum(len(files[i]['t']) for i in idx)
        last = max(idx, key=lambda i: files[i]['t'][-1])       # the clock: t_s of the last save's last sample
        rec['clock'] = (files[last]['saved'], files[last]['t'][-1])
        recs.append(rec)
    return recs


def load_runs(folder, min_len=1200):
    """The runs of every CSV under `folder`, each a dict of 1 s arrays (t, p,
    v, A, D, sp, mode, stage) with 'group' (what is held out together), 'rec'
    and 'name' (local time of its first sample)."""
    paths = glob.glob(os.path.join(folder, '**', 'pmc_session_*.csv'), recursive=True)
    if not paths:
        raise SystemExit('no pmc_session_*.csv under %s' % folder)
    recs = recordings(paths)
    runs, group = [], -1
    for k, r in enumerate(recs):
        ok = ~np.isnan(r['v']) & ~np.isnan(r['p']) & np.isin(r['mode'], ['MANUAL', 'AUTO'])
        t = r['t']
        brk = np.where((np.diff(t) > 5) | (ok[1:] != ok[:-1]))[0] + 1
        prev_end = None
        for s in np.split(np.arange(len(t)), brk):
            if not ok[s[0]] or t[s[-1]] - t[s[0]] < min_len or np.nanmax(r['v'][s]) <= 0:
                continue
            grid = np.arange(np.ceil(t[s[0]]), np.floor(t[s[-1]]) + 1)
            run = {'t': grid, 'rec': k, 'files': r['files']}
            for c in ('p', 'v', 'A', 'D', 'sp'):
                y = r[c][s]
                good = ~np.isnan(y)
                run[c] = np.interp(grid, t[s][good], y[good]) if good.sum() > 1 else np.full(len(grid), np.nan)
            idx = np.clip(np.searchsorted(t[s], grid), 0, len(s) - 1)
            run['mode'], run['stage'] = r['mode'][s][idx], r['stage'][s][idx]
            if prev_end is None or grid[0] - prev_end > 3600:
                group += 1
            prev_end = grid[-1]
            run['group'] = group
            saved, t_saved = r['clock']
            run['start'] = saved - datetime.timedelta(seconds=float(t_saved - grid[0])) if saved else None
            run['name'] = run['start'].strftime('%-d %b %H:%M') if saved else 'rec %d t %d' % (k, grid[0])
            runs.append(run)
    return runs, recs


if __name__ == '__main__':
    import sys
    runs, recs = load_runs(sys.argv[1])
    print('%d files, %d samples, %d after removing the shared ones' % (
        sum(len(r['files']) for r in recs), sum(r['n_raw'] for r in recs), sum(len(r['t']) for r in recs)))
    for r in runs:
        print('group %d  %-12s %5d s  P %5.2f to %5.2f mbar  valve %5.2f to %5.2f %%  heat exchanger %5.1f to %5.1f K  %s' % (
            r['group'], r['name'], len(r['t']), r['p'].min(), r['p'].max(), r['v'].min(), r['v'].max(),
            np.nanmin(r['A']), np.nanmax(r['A']), ', '.join(f[12:-4] for f in r['files'])))
