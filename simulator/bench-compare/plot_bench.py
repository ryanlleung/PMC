#!/usr/bin/env python3
"""Draws a bench-replay.js result: the bench log and the simulator, same valve moves.

    python3 plot_bench.py replay.csv out.png "title" [--log] [--from S] [--to S] [--seconds]

Three panels: pressure, valve and heat exchanger, bench and simulator in
each; the autotune's reach and relay stages are shaded and named. Needs
matplotlib.
"""
import argparse
import csv
import re

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt   # noqa: E402

BENCH, SIM = '#2a78d6', '#eb6834'
INK, MUTED, GRID, SHADE = '#1f1f1e', '#6b6a64', '#e6e5e0', '#f1f0ec'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv'); ap.add_argument('png'); ap.add_argument('title')
    ap.add_argument('--log', action='store_true', help='log pressure axis')
    ap.add_argument('--from', dest='t0', type=float); ap.add_argument('--to', dest='t1', type=float)
    ap.add_argument('--seconds', action='store_true', help='time in s, not min')
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.csv, newline=''))
            if (a.t0 is None or float(r['t_s']) >= a.t0) and (a.t1 is None or float(r['t_s']) <= a.t1)]
    col = lambda k: [float(r[k]) if r[k] != '' else float('nan') for r in rows]
    unit = 1.0 if a.seconds else 60.0
    base = float(rows[0]['t_s'])
    t = [(float(r['t_s']) - base) / unit for r in rows]
    fig, ax = plt.subplots(3, 1, figsize=(12, 8.2), sharex=True, gridspec_kw={'height_ratios': [3, 1.3, 1.3]})
    ax[0].plot(t, col('bench_p'), color=BENCH, lw=1.2, label='bench (your log)')
    ax[0].plot(t, col('sim_p'), color=SIM, lw=1.2, label='simulator, the same valve moves')
    if a.log:
        ax[0].set_yscale('log')
        ax[0].set_yticks([0.7, 1, 2, 4, 8, 12, 16, 24, 32])
        ax[0].yaxis.set_major_formatter(matplotlib.ticker.FormatStrFormatter('%g'))
        ax[0].yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    ax[0].set_ylabel('pressure, mbar', color=INK)
    ax[0].legend(loc='lower left', bbox_to_anchor=(0, 1.07), ncol=2, frameon=False, fontsize=10,
                 borderaxespad=0, handlelength=1.6)
    ax[0].set_title(a.title, loc='left', fontsize=12, color=INK, pad=38)
    ax[1].plot(t, col('bench_valve'), color=BENCH, lw=1.2)
    ax[1].plot(t, col('sim_valve'), color=SIM, lw=1.0)
    ax[1].set_ylabel('valve, %', color=INK)
    ax[2].plot(t, col('bench_hx'), color=BENCH, lw=1.2)
    ax[2].plot(t, col('sim_hx'), color=SIM, lw=1.2)
    ax[2].set_ylabel('heat exchanger, K', color=INK)
    ax[2].set_xlabel('s' if a.seconds else 'min', color=INK)
    for x in ax:
        x.grid(True, color=GRID, lw=0.6)
        x.set_axisbelow(True)
        for s in ('top', 'right'):
            x.spines[s].set_visible(False)
        for s in ('left', 'bottom'):
            x.spines[s].set_color('#b9b8b0')
        x.tick_params(colors=MUTED, labelsize=9)
    # Shade and name the reach and relay stages (the checks stay plain).
    spans, prev, start = [], None, None
    for r, tt in zip(rows, t):
        m = re.match(r'([\d.]+) mbar (reach|relay)', r['stage'])
        key = m.group(0) if m else None
        if key != prev:
            if prev:
                spans.append((start, tt, prev))
            prev, start = key, tt
    if prev:
        spans.append((start, t[-1], prev))
    for s0, s1, name in spans:
        for x in ax:
            x.axvspan(s0, s1, color=SHADE, lw=0, zorder=0)
        if s1 - s0 < 0.03 * (t[-1] - t[0]):
            continue                 # too narrow to name
        ax[0].text((s0 + s1) / 2, 1.005, name.replace(' mbar', ''), transform=ax[0].get_xaxis_transform(),
                   ha='center', va='bottom', fontsize=8, color=MUTED)
    fig.tight_layout()
    fig.savefig(a.png, dpi=110, facecolor='white')


if __name__ == '__main__':
    main()
