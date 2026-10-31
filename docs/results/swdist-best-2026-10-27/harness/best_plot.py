#!/usr/bin/env python3
"""Forwarded Mpps against the per-packet work, one panel per core count: the best
split distributor and the single-master distributor (this campaign's sat.csv)
with mu_sq and plain RSS (the 2026-10-27 work matrix) as lines (fixed work)
and hollow markers (exp work, same mean).
  split_plot.py [sat.csv] [reference sat.csv] [out.png]"""
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'sat', 'sat.csv')
ref = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', '..', 'musq-vs-swdist-work-2026-10-27', 'sat', 'sat.csv')
out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, '..', 'swdist-best.png')
SERIES = [('best', 'best distributor, 1 master (+ its own share) + N-1 workers', '#8a3ffc'),
          ('dist', 'distributor, 1 master + N-1 workers', '#eb6834'),
          ('musq', 'mu_sq, N lanes', '#2a78d6'),
          ('plain', 'plain RSS, N cores (no ordering)', '#1baf7a')]
INK, INK2, GRID, SURF = '#0b0b0b', '#52514e', '#e4e3df', '#fcfcfb'
data = defaultdict(dict)  # (cores, mode, kind) -> ns -> fwd
primary = set()
for r in csv.DictReader(open(src)):
    kind, ns = r['work'].split(':')
    data[(int(r['cores']), r['mode'], kind)][int(ns)] = float(r['fwd_mpps'])
    primary.add((int(r['cores']), r['mode'], kind, int(ns)))
cores_list = sorted({k[0] for k in data})
for r in csv.DictReader(open(ref)):
    kind, ns = r['work'].split(':')
    key = (int(r['cores']), r['mode'], kind, int(ns))
    if r['mode'] in ('musq', 'plain', 'dist') and int(r['cores']) in cores_list and key not in primary:
        data[(int(r['cores']), r['mode'], kind)][int(ns)] = float(r['fwd_mpps'])
levels = sorted({ns for k, v in data.items() if k[2] == 'fixed' for ns in v})
xpos = {ns: i for i, ns in enumerate(levels)}
plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10, 'axes.edgecolor': GRID, 'axes.labelcolor': INK2,
                     'xtick.color': INK2, 'ytick.color': INK2, 'text.color': INK})
fig, axes = plt.subplots(1, len(cores_list), figsize=(4.6 * len(cores_list), 4.6), sharey=True, facecolor=SURF)
ymax = max(v for k, d in data.items() for v in d.values())
for ax, cores in zip(axes, cores_list):
    ax.set_facecolor(SURF)
    placed = []
    for mode, label, color in SERIES:
        d = data.get((cores, mode, 'fixed'), {})
        xs = [xpos[ns] for ns in levels if ns in d]
        ys = [d[ns] for ns in levels if ns in d]
        ax.plot(xs, ys, color=color, linewidth=2, marker='o', markersize=6, markeredgecolor=SURF, markeredgewidth=1,
                label=label if cores == cores_list[0] else None, zorder=3)
        e = data.get((cores, mode, 'exp'), {})
        if e:
            ax.plot([xpos[ns] for ns in e], list(e.values()), linestyle='none', marker='o', markersize=8,
                    markerfacecolor=SURF, markeredgecolor=color, markeredgewidth=2, zorder=4,
                    label='hollow: exp work, same mean' if (cores == cores_list[0] and mode == 'best') else None)
        if ys:
            # left-end value label; nudged apart when two series start within 2.5 Mpps
            dy = 0
            for y0 in placed:
                if abs(y0 - ys[0]) < 2.5:
                    dy = -6 if ys[0] < y0 else 6
            placed.append(ys[0])
            ax.annotate(f'{ys[0]:.0f}', (xs[0], ys[0]), xytext=(-4, dy), textcoords='offset points', fontsize=8,
                        color=INK2, va='center', ha='right')
    ax.set_title(f'{cores} cores', color=INK, fontsize=11, loc='left')
    ax.set_xticks(list(xpos.values()))
    ax.set_xticklabels([str(ns) for ns in levels])
    ax.set_xlim(-0.6, len(levels) - 0.4)
    ax.grid(axis='y', color=GRID, linewidth=0.8)
    for s in ('top', 'right'):
        ax.spines[s].set_visible(False)
    ax.tick_params(length=0)
axes[0].set_ylabel('forwarded, Mpps (128 B, 80 Mpps offered)')
fig.supxlabel('mock work per packet, ns (nominal; the spin costs 30 to 90 cycles more than that)', color=INK2, fontsize=10)
axes[0].set_ylim(0, ymax * 1.08)
fig.legend(loc='upper center', ncol=3, frameon=False, bbox_to_anchor=(0.5, 1.02), fontsize=9)
fig.suptitle('improved distributor (master works too) vs finding 18 at equal cores, 2026-10-27 (saturation runs)',
             y=1.1, fontsize=11, color=INK)
fig.tight_layout()
fig.savefig(out, dpi=130, bbox_inches='tight', facecolor=SURF)
print(out)
