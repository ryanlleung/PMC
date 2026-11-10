"""Plots for fit.py: each held-out run (bench, physics, learned) and the error by pressure."""
import os

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

INK, MUTED, GRID, SURFACE = '#0b0b0b', '#898781', '#e1e0d9', '#fcfcfb'
PHYS, LEARN, AUX = '#eb6834', '#2a78d6', '#1baf7a'
plt.rcParams.update({'font.size': 11, 'axes.edgecolor': '#c3c2b7', 'axes.labelcolor': '#52514e', 'xtick.color': MUTED,
                     'ytick.color': MUTED, 'axes.spines.top': False, 'axes.spines.right': False, 'figure.facecolor': SURFACE,
                     'axes.facecolor': SURFACE, 'savefig.facecolor': SURFACE, 'axes.grid': True, 'grid.color': GRID,
                     'grid.linewidth': 0.6, 'axes.titlesize': 12, 'axes.titlelocation': 'left'})


def _panel(ax, t, series, ylabel, log=False):
    for y, color, label, lw in series:
        ax.plot(t, y, color=color, lw=lw, label=label)
    ax.set_ylabel(ylabel)
    if log:
        ax.set_yscale('log')
        ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: '%g' % v))
        ax.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    if len(series) > 1:
        ax.legend(loc='lower left', ncol=len(series), frameon=False, fontsize=10, bbox_to_anchor=(0, 1.0), borderaxespad=0.2)


def overlays(runs, out, scored, key='ml valve + cold head'):
    for r in runs:
        k = scored(r)
        if k.sum() < 300:
            continue
        t = (r['t'] - r['t'][0]) / 60
        e = lambda m: np.sqrt(np.mean((m[k] - r['p'][k]) ** 2))
        fig, ax = plt.subplots(4, 1, figsize=(13, 10), sharex=True, gridspec_kw=dict(height_ratios=[3, 1.1, 1.1, 1.1], hspace=0.36))
        _panel(ax[0], t, [(r['p'], INK, 'bench', 1.6), (r['pm'], PHYS, 'physics model (rms %.2f mbar)' % e(r['pm']), 1.3),
                          (r[key], LEARN, 'learned model, valve + cold head (rms %.2f mbar)' % e(r[key]), 1.3)], 'pressure, mbar', log=True)
        ax[0].set_title('Run of %s, left out of the training: pressure from the logged valve' % r['name'], pad=22)
        _panel(ax[1], t, [(r['v'], INK, 'valve', 1.3)], 'valve, %')
        _panel(ax[2], t, [(r['A'], INK, 'bench', 1.6), (r['am'], PHYS, 'physics model', 1.3)], 'heat exchanger, K')
        _panel(ax[3], t, [(r['D'], INK, 'cold head', 1.3)], 'cold head, K')
        ax[3].set_xlabel('minutes from the start of the run')
        fig.savefig(os.path.join(out, 'held-out-%s.png' % r['start'].strftime('%m%d-%H%M')), dpi=110, bbox_inches='tight')
        plt.close(fig)


def summary(T, bands, out):
    names, series = [], {'physics model': [], 'learned, valve only': [], 'learned, valve + cold head': []}
    for lo, hi, name in bands:
        k = (T['p'] >= lo) & (T['p'] < hi)
        if not k.sum():
            continue
        names.append(name)
        for label, key in (('physics model', 'pm'), ('learned, valve only', 'ml valve'), ('learned, valve + cold head', 'ml valve + cold head')):
            series[label].append(float(np.median(np.abs(T[key][k] - T['p'][k]) / T['p'][k]) * 100))
    fig, ax = plt.subplots(figsize=(9, 4.6))
    x = np.arange(len(names))
    for i, (label, color) in enumerate(zip(series, (PHYS, LEARN, AUX))):
        bars = ax.bar(x + (i - 1) * 0.27, series[label], 0.25, color=color, label=label)
        for b, v in zip(bars, series[label]):
            ax.text(b.get_x() + b.get_width() / 2, v + 0.4, '%.0f' % v, ha='center', va='bottom', fontsize=9, color='#52514e')
    ax.set_xticks(x, names)
    ax.set_xlabel('bench pressure, mbar')
    ax.set_ylabel('median error, % of the pressure')
    ax.grid(axis='x', visible=False)
    ax.set_title('Held-out runs: how far each model is from the bench pressure', pad=26)
    ax.legend(loc='lower left', ncol=3, frameon=False, fontsize=10, bbox_to_anchor=(0, 1.0), borderaxespad=0.2)
    fig.savefig(os.path.join(out, 'error-by-pressure.png'), dpi=110, bbox_inches='tight')
    plt.close(fig)
