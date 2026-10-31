#!/usr/bin/env python3
"""Slide-sized charts for the results deck (deck background, >= 24 px text at
1088 px width).  plot_slides.py <results_dir> <lane_epoch_dir> <out_dir>
  slide_ci_pi.png    lane 0's wqe_ci - wqe_pi over 2 ms, 2 cores, 256 B, at
                     17 / 19 / 21 Mpps per core (lane_c2_f256_r{34,38,42}.csv)
  slide_ceiling.png  max Mpps (lines, left) and average CPU busy % (hollow,
                     right) vs frame size, from runs.csv
Also prints the per-rate statistics for the CI-PI slide's table."""
import csv
import gzip
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402

R, E, OUT = sys.argv[1:4]
BG, INK, INK2, MUTED, GRID, AXIS = '#F5F3EE', '#14202E', '#4A5563', '#5E6672', '#DEDAD0', '#9AA1A9'
plt.rcParams.update({'font.family': ['DejaVu Sans'], 'font.size': 17, 'text.color': INK,
                     'axes.labelcolor': INK2, 'xtick.color': MUTED, 'ytick.color': MUTED,
                     'axes.edgecolor': AXIS, 'axes.linewidth': 1.4})


def style(ax):
    ax.set_facecolor(BG)
    ax.grid(axis='y', color=GRID, linewidth=1.4)
    ax.set_axisbelow(True)
    for s in ('top', 'right', 'left'):
        ax.spines[s].set_visible(False)
    ax.tick_params(length=0, pad=8)


def lane_log(path):
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rt') as f:
        rows = [x for x in csv.reader(l for l in f if not l.startswith('#'))]
    idx = {h: i for i, h in enumerate(rows[0])}
    lanes = defaultdict(list)
    for x in rows[1:]:
        lanes[x[0]].append(x)
    out = {}
    for lane, L in lanes.items():
        act = [i for i in range(1, len(L)) if int(L[i][idx['rx_pkts']]) > int(L[i - 1][idx['rx_pkts']])]
        out[lane] = L[act[0] + 40: act[-1] - 40]  # steady part: 1 ms off each end
    return out, idx


# --- CI - PI over time -------------------------------------------------------
RATES = [(34, 17, '#2B5F9E'), (38, 19, '#5E6672'), (42, 21, '#B4501A')]
ALL = [34, 38, 40, 42]
stats = {}
series = {}
for total in ALL:
    p = os.path.join(E, f'lane_c2_f256_r{total}.csv')
    lanes, idx = lane_log(p if os.path.exists(p) else p + '.gz')
    S = lanes['0']
    d = [(int(x[idx['wqe_ci']]) - int(x[idx['wqe_pi']])) & 0xffff for x in S]
    ds = sorted(d)
    n = len(ds)
    refuse_ep = sum(1 for i in range(1, n) if int(S[i][idx['tx_drops']]) > int(S[i - 1][idx['tx_drops']]))
    busy = [float(x[idx['busy_pct']]) for x in S]
    stats[total] = dict(p50=ds[n // 2], p99=ds[99 * n // 100], refuse_ep=100 * refuse_ep / n)
    # 2 ms window starting 2 s into the steady part
    t = [float(x[idx['t_us']]) for x in S]
    i0 = next(i for i in range(n) if t[i] - t[0] >= 2e6)
    series[total] = ([t[i] - t[i0] for i in range(i0, i0 + 81)], d[i0:i0 + 81])

fig, ax = plt.subplots(figsize=(11.0, 6.2), dpi=200, facecolor=BG)
style(ax)
ax.axhline(2048, color=INK, linewidth=1.6, linestyle=(0, (6, 4)))
ax.text(0, 2048 + 40, 'ring size: 2048 WQEBBs', color=INK, fontsize=17, va='bottom')
for total, per_core, color in RATES:
    xs, ys = series[total]
    ax.plot(xs, ys, color=color, linewidth=2.2, solid_joinstyle='round')
    ax.text(2040, stats[total]['p50'], f'{per_core} Mpps/core', color=INK, fontsize=17, va='center')
ax.set_xlim(0, 2000)
ax.set_ylim(0, 2300)
ax.set_yticks([0, 512, 1024, 1536, 2048])
ax.set_xticks([0, 500, 1000, 1500, 2000])
ax.set_xticklabels(['0', '0.5', '1.0', '1.5', '2.0 ms'])
ax.set_ylabel('wqe_ci − wqe_pi (WQEBBs)')
for k in ('right',):
    ax.spines[k].set_visible(False)
fig.subplots_adjust(left=0.11, right=0.80, top=0.95, bottom=0.10)
fig.savefig(os.path.join(OUT, 'slide_ci_pi.png'), facecolor=BG)
for total in ALL:
    print(total, total / 2, stats[total])

# --- ceiling: Mpps lines + hollow CPU markers --------------------------------
FRAMES = [98, 128, 256, 512, 1024, 1518]
STY = {2: ('#2B5F9E', 'o'), 4: ('#C8692A', 's'), 8: ('#1F8A65', '^')}
passing = defaultdict(list)
with open(os.path.join(R, 'runs.csv')) as f:
    for r in csv.DictReader(f):
        if r['pass'] == 'True':
            passing[(int(r['cores']), int(r['frame']), float(r['target_mpps']))].append(
                (float(r['tx_mpps']), float(r['cpu_avg'])))
point = {}
for (c, fr, tgt), v in passing.items():
    if len(v) >= 3 and tgt > point.get((c, fr), (0,))[0]:
        point[(c, fr)] = (tgt, sum(a for a, _ in v) / len(v), sum(b for _, b in v) / len(v))
fig, ax = plt.subplots(figsize=(10.88, 6.2), dpi=200, facecolor=BG)
style(ax)
axc = ax.twinx()
axc.set_ylim(0, 100)
axc.set_yticks(range(0, 101, 20))
axc.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f'{v:.0f} %'))
for s in axc.spines.values():
    s.set_visible(False)
axc.tick_params(length=0, pad=8, colors=MUTED)
axc.set_ylabel('CPU busy % (hollow)', color=INK2, rotation=270, labelpad=26)
dodge = {2: -0.13, 4: 0.0, 8: 0.13}
for c, (col, mk) in STY.items():
    xs = [FRAMES.index(f) + dodge[c] for f in FRAMES]
    ax.plot(xs, [point[(c, f)][1] for f in FRAMES], color=col, linewidth=2.6, marker=mk, markersize=11,
            markeredgecolor=BG, markeredgewidth=1.8, zorder=3)
    axc.plot(xs, [point[(c, f)][2] for f in FRAMES], linestyle='none', marker=mk, markersize=11,
             markerfacecolor=BG, markeredgecolor=col, markeredgewidth=2.2, zorder=3)
ax.set_xticks(range(len(FRAMES)))
ax.set_xticklabels([f'{f} B' for f in FRAMES])
ax.set_xlim(-0.45, len(FRAMES) - 0.55)
ax.set_ylim(0, 50)
ax.set_yticks(range(0, 51, 10))
ax.set_ylabel('Mpps before TX overflows (lines)')
h = [Line2D([], [], color=STY[k][0], marker=STY[k][1], markersize=11, linewidth=2.6, markeredgecolor=BG,
            label=f'{k} cores') for k in STY]
h.append(Line2D([], [], linestyle='none', marker='o', markersize=11, markerfacecolor=BG, markeredgecolor=MUTED,
                markeredgewidth=2.2, label='CPU busy % (right)'))
axc.legend(handles=h, loc='upper right', frameon=False, fontsize=17, labelcolor=INK2, borderaxespad=0.2)
fig.subplots_adjust(left=0.09, right=0.86, top=0.97, bottom=0.10)
fig.savefig(os.path.join(OUT, 'slide_ceiling.png'), facecolor=BG)
