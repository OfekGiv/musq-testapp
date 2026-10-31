#!/usr/bin/env python3
"""pps vs frame size from runs.csv, one line per core count, each line with its
own marker shape. Point = mean forwarded rate (tx_mpps) of the passing runs at
the highest rate that passed 3 runs (solid, left axis); hollow markers, right axis =
their mean CPU busy % (average over the lane cores).  plot_pps.py <results_dir>"""
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.lines  # noqa: E402,F401
import matplotlib.ticker  # noqa: E402,F401

R = sys.argv[1]
FRAMES = [98, 128, 256, 512, 1024, 1518]
STYLE = {2: ('#2a78d6', 'o'), 4: ('#eb6834', 's'), 8: ('#1baf7a', '^')}  # palette slots 1-3, one shape per line
SURFACE, INK, INK2, MUTED, GRID, AXIS = '#fcfcfb', '#0b0b0b', '#52514e', '#898781', '#e1e0d9', '#c3c2b7'

passing = defaultdict(list)  # (cores, frame, target) -> (tx_mpps, cpu_avg) of passing runs
with open(os.path.join(R, 'runs.csv')) as f:
    for r in csv.DictReader(f):
        if r['pass'] == 'True':
            passing[(int(r['cores']), int(r['frame']), float(r['target_mpps']))].append(
                (float(r['tx_mpps']), float(r['cpu_avg'])))
point = {}
for (cores, frame, target), v in passing.items():
    if len(v) >= 3 and target > point.get((cores, frame), (0, 0))[0]:
        point[(cores, frame)] = (target, sum(t for t, _ in v) / len(v), sum(c for _, c in v) / len(v))

plt.rcParams.update({'font.family': ['DejaVu Sans'], 'font.size': 10, 'text.color': INK, 'axes.labelcolor': INK2,
                     'xtick.color': MUTED, 'ytick.color': MUTED, 'axes.edgecolor': AXIS})
fig, ax = plt.subplots(figsize=(9, 5.6), facecolor=SURFACE)
ax.set_facecolor(SURFACE)
ax.grid(axis='y', color=GRID, linewidth=1, linestyle='-')
ax.set_axisbelow(True)
for s in ('top', 'right', 'left'):
    ax.spines[s].set_visible(False)
ax.tick_params(length=0)
dodge = {2: -0.12, 4: 0.0, 8: 0.12}  # equal values (2 vs 8 cores, 4-core pps vs 2-core CPU at 98-128 B) side by side
axc = ax.twinx()  # average CPU busy %
axc.set_ylim(0, 100)
axc.set_yticks(range(0, 101, 20))
axc.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f'{v:.0f}%'))
for sp in axc.spines.values():
    sp.set_visible(False)
axc.tick_params(length=0)
axc.set_ylabel('Average CPU busy % of the lane cores (hollow markers)', rotation=270, labelpad=16)
for cores, (color, marker) in STYLE.items():
    fs = [f for f in FRAMES if (cores, f) in point]
    ax.plot([FRAMES.index(f) + dodge[cores] for f in fs], [point[(cores, f)][1] for f in fs],
            color=color, linewidth=2, marker=marker, markersize=9, markeredgecolor=SURFACE, markeredgewidth=1.5,
            solid_joinstyle='round', solid_capstyle='round', label=f'{cores} cores ({cores} lanes)', zorder=3)
    axc.plot([FRAMES.index(f) + dodge[cores] for f in fs], [point[(cores, f)][2] for f in fs],
             color=color, linestyle='none', marker=marker, markersize=8,
             markerfacecolor=SURFACE, markeredgecolor=color, markeredgewidth=1.5, zorder=3)
ax.set_xticks(range(len(FRAMES)))
ax.set_xticklabels([str(f) if f != 1518 else '1518\n(MTU)' for f in FRAMES])
ax.set_xlim(-0.4, len(FRAMES) - 0.6)
ax.set_ylim(0, 50)  # 0-50M with 0-100 %: each gridline is 10 Mpps and 20 %
ax.set_yticks(range(0, 51, 10))
ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f'{v:.0f}M'))
ax.set_xlabel('Frame size (B)')
ax.set_ylabel('Packets per second (lines, filled markers)')
ax.set_title('mu_sq forwarding rate before the TX queue overflows', loc='left', fontsize=12, color=INK, pad=30)
ax.text(0, 1.04, 'Highest rate with TX refused and RX lost <= 0.01 % (mean of 3 runs), and the CPU busy % at that rate', transform=ax.transAxes, fontsize=9, color=INK2, va='bottom')
h = [matplotlib.lines.Line2D([], [], linestyle='none', color=STYLE[k][0], marker=STYLE[k][1], markersize=8,
                              label=f'{k} cores ({k} lanes)') for k in STYLE]
h += [matplotlib.lines.Line2D([], [], color=MUTED, linewidth=2, label='packets per second (left axis)'),
      matplotlib.lines.Line2D([], [], color=MUTED, linestyle='none', marker='o', markersize=7,
                              markerfacecolor=SURFACE, markeredgecolor=MUTED, label='average CPU busy % (right axis)')]
axc.legend(handles=h, loc='upper right', frameon=False, fontsize=9, labelcolor=INK2)
fig.tight_layout()
out = os.path.join(R, 'musq-ppw1-pps-vs-frame.png')
fig.savefig(out, dpi=150, facecolor=SURFACE)
for k in sorted(point):
    print(k, round(point[k][1], 3), round(point[k][2], 1))
print(out)
