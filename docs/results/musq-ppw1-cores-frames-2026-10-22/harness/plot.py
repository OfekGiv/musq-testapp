#!/usr/bin/env python3
"""summary.csv + PNG from cases/*.json: max Mpps before the mu_sq TX queue
overflows vs frame size (top), CPU busy % at that rate (bottom), one series per
core count.  plot.py [out_dir]"""
import csv
import glob
import json
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else HERE
FRAMES = [98, 128, 256, 512, 1024, 1518]
SERIES = {2: '#2a78d6', 4: '#eb6834', 8: '#1baf7a'}   # reference palette slots 1-3 (light)
SURFACE, INK, INK2, MUTED, GRID, AXIS = '#fcfcfb', '#0b0b0b', '#52514e', '#898781', '#e1e0d9', '#c3c2b7'

cases = [json.load(open(p)) for p in sorted(glob.glob(os.path.join(HERE, 'cases', '*.json')))]
cases.sort(key=lambda c: (c['cores'], c['frame']))
# first-fail fields from runs.csv: the failing runs at first_fail_mpps (a case
# whose confirm step moved down also has a passing run at that rate)
with open(os.path.join(HERE, 'runs.csv')) as f:
    runs = list(csv.DictReader(f))
for c in cases:
    fr = [r for r in runs if int(r['cores']) == c['cores'] and int(r['frame']) == c['frame']
          and abs(float(r['target_mpps']) - c['first_fail_mpps']) < 1e-6 and r['pass'] == 'False']
    if fr:
        c['first_fail_limit'] = fr[0]['limit']
        c['first_fail_tx_refused_pct'] = round(sum(float(r['tx_refused_pct']) for r in fr) / len(fr), 3)
        c['first_fail_rx_loss_pct'] = round(sum(float(r['rx_loss_pct']) for r in fr) / len(fr), 3)
cols = ['cores', 'frame', 'wire_bytes', 'max_mpps', 'forwarded_mpps', 'gbps_wire', 'cpu_avg', 'cpu_max',
        'cpu_peak_1s', 'cyc_per_pkt', 'first_fail_mpps', 'first_fail_limit', 'first_fail_tx_refused_pct',
        'first_fail_rx_loss_pct', 'runs', 'busy_per_lcore']
with open(os.path.join(OUT, 'musq-ppw1-cores-frames-summary.csv'), 'w', newline='') as f:
    w = csv.DictWriter(f, cols)
    w.writeheader()
    for c in cases:
        w.writerow({k: (json.dumps(c[k]) if k == 'busy_per_lcore' else c[k]) for k in cols})

plt.rcParams.update({'font.family': ['DejaVu Sans'], 'font.size': 10, 'text.color': INK,
                     'axes.labelcolor': INK2, 'xtick.color': MUTED, 'ytick.color': MUTED,
                     'axes.edgecolor': AXIS, 'axes.linewidth': 1})
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 8.4), sharex=True, gridspec_kw={'height_ratios': [1.25, 1]},
                               facecolor=SURFACE)
xpos = {f: i for i, f in enumerate(FRAMES)}
dodge = {2: -0.12, 4: 0.0, 8: 0.12}
for ax in (ax1, ax2):
    ax.set_facecolor(SURFACE)
    ax.grid(axis='y', color=GRID, linewidth=1, linestyle='-')
    ax.set_axisbelow(True)
    for s in ('top', 'right', 'left'):
        ax.spines[s].set_visible(False)
    ax.tick_params(length=0)

for cores, color in SERIES.items():
    cs = [c for c in cases if c['cores'] == cores]
    if not cs:
        continue
    xs = [xpos[c['frame']] + dodge[cores] for c in cs]
    ax1.plot(xs, [c['max_mpps'] for c in cs], color=color, linewidth=2, solid_joinstyle='round',
             solid_capstyle='round', zorder=2)
    for c, x in zip(cs, xs):
        tx = 'TX' in c['first_fail_limit']
        ax1.plot(x, c['max_mpps'], 'o', markersize=9, markeredgewidth=2, zorder=3,
                 markerfacecolor=color if tx else SURFACE, markeredgecolor=color)
    last = cs[-1]
    xd = xs
    for c, x in zip(cs, xd):
        ax2.plot([x, x], [c['cpu_avg'], c['cpu_max']], color=color, linewidth=2, solid_capstyle='round', zorder=2)
    ax2.plot(xd, [c['cpu_avg'] for c in cs], 'o', color=color, markersize=8, markeredgecolor=SURFACE,
             markeredgewidth=2, zorder=3)
    ax2.plot(xd, [c['cpu_max'] for c in cs], '_', color=color, markersize=10, markeredgewidth=2, zorder=3)

top = max((c['max_mpps'] for c in cases), default=10)
ax1.set_ylim(0, 5 * (int(top * 1.2) // 5 + 1))
ax1.set_ylabel('Mpps')
ax1.set_title('Highest offered rate forwarded before the TX queue overflows', loc='left', fontsize=12, color=INK, pad=10)
ax2.set_ylim(0, 105)
ax2.set_ylabel('CPU busy %')
ax2.set_title('CPU busy % per lane core at that rate: dot = average over cores, bar top = busiest core',
              loc='left', fontsize=10.5, color=INK, pad=8)
ax2.set_xticks(range(len(FRAMES)))
ax2.set_xticklabels([str(f) if f != 1518 else '1518\n(MTU)' for f in FRAMES])
ax2.set_xlabel('Frame size requested (B); NIC ESP encapsulation, wire 98 / 126 / 254 / 510 / 1022 / 1518 B')
ax2.set_xlim(-0.4, len(FRAMES) - 0.6)

handles = [Line2D([], [], color=SERIES[k], linewidth=2, marker='o', markersize=8, label=f'{k} cores ({k} lanes)')
           for k in SERIES if any(c['cores'] == k for c in cases)]
all_tx = all('TX' in c['first_fail_limit'] for c in cases)
if all_tx:
    ax1.text(0.0, 1.0, 'At every point the next 0.5 Mpps (0.25 from 1024 B) overflowed on TX; nothing was lost on RX',
             transform=ax1.transAxes, fontsize=9, color=INK2, va='bottom')
    ax1.set_title(ax1.get_title(loc='left'), loc='left', fontsize=12, color=INK, pad=22)
else:
    handles += [Line2D([], [], linestyle='none', marker='o', markersize=8, markerfacecolor=MUTED, markeredgecolor=MUTED,
                   markeredgewidth=2, label='next step: TX queue overflowed'),
            Line2D([], [], linestyle='none', marker='o', markersize=8, markerfacecolor=SURFACE, markeredgecolor=MUTED,
                   markeredgewidth=2, label='next step: lost on RX first (port)')]
leg = ax1.legend(handles=handles, loc='upper right', frameon=False, fontsize=9, labelcolor=INK2)
fig.text(0.01, 0.005, 'mu_sq responder, danger51 (ConnectX-9), 1 packet per WQE, one lane per core, '
         'rxd 1024, txd 1024 x lanes,\nmu_sq_comp_n 128, db-wqes 128, prom-dpdk -O3. '
         'Each point passed 3 runs (TX refused and RX lost each <= 0.01 %).\nBusy % = share of TSC cycles spent in loop passes that handled packets (app counters).', fontsize=7.5, color=MUTED)
fig.tight_layout(rect=(0, 0.05, 1, 1))
png = os.path.join(OUT, 'musq-ppw1-cores-frames.png')
fig.savefig(png, dpi=150, facecolor=SURFACE)
print(png, len(cases), 'cases')
