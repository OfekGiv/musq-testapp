#!/usr/bin/env python3
"""Markdown table: the SW distributor cases (cases/*.json) next to the mu_sq
finding-16 cases of the same frame sizes.  compare.py [musq summary csv]"""
import csv
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# compare.py [sweep output dir (default: this dir)] [mu_sq summary csv]
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else HERE
MUSQ = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    HERE, '..', '..', 'musq-ppw1-cores-frames-2026-10-22', 'musq-ppw1-cores-frames-summary.csv')

sys.path.insert(0, HERE)
from parse_run import parse  # noqa: E402

sw = [json.load(open(p)) for p in sorted(glob.glob(os.path.join(OUT, 'cases', '*.json')))]
sw.sort(key=lambda c: (c['cores'], c['frame']))
# The case's cyc_per_pkt averages master and workers; take the master's own
# (lcore 2) from the passing runs at the reported rate.
runs_csv = os.path.join(OUT, 'runs.csv')
runs = list(csv.DictReader(open(runs_csv))) if os.path.exists(runs_csv) else []
for c in sw:
    vals = []
    for r in runs:
        if int(r['cores']) == c['cores'] and int(r['frame']) == c['frame'] and r['pass'] == 'True' \
                and abs(float(r['target_mpps']) - c['max_mpps']) < 1e-6:
            path = os.path.join(OUT, 'runs', f"resp_{r['tag']}.csv")
            if os.path.exists(path):
                vals.append(parse(path, float(r['target_mpps']))['cyc_per_pkt'].get('lcore2', 0.0))
    if vals:
        c['cyc_per_pkt_master'] = sum(vals) / len(vals)
musq = list(csv.DictReader(open(MUSQ)))
frames = sorted({c['frame'] for c in sw})


def busy_master(c):
    b = c['busy_per_lcore']
    return b.get('lcore2', 0.0)


def busy_workers(c):
    b = [v for k, v in c['busy_per_lcore'].items() if k != 'lcore2']
    return (min(b), max(b)) if b else (0.0, 0.0)


print('| frame (wire) | SW 1 master + %s: max Mpps, Gbps | master busy %%, cycles/pkt | workers busy %% | first fail | '
      'mu_sq 2 cores (finding 16) | mu_sq 4 cores | mu_sq 8 cores |' % ' / '.join(str(c['cores'] - 1) for c in sorted({c['cores']: c for c in sw}.values(), key=lambda c: c['cores'])))
print('|---|---|---|---|---|---|---|---|')
for f in frames:
    cs = [c for c in sw if c['frame'] == f]
    for c in cs:
        mrows = {int(r['cores']): r for r in musq if int(r['frame']) == f}
        wb = busy_workers(c)
        cyc = c.get('cyc_per_pkt_master', c['cyc_per_pkt'])
        mu = ' | '.join(f"{float(mrows[k]['max_mpps']):g} Mpps, busy {float(mrows[k]['cpu_avg']):.0f} %" if k in mrows else '-'
                        for k in (2, 4, 8))
        print(f"| {f} ({c['wire_bytes']:g}) | **{c['max_mpps']:g} Mpps**, {c['gbps_wire']:g} Gbps "
              f"({c['cores'] - 1} workers) | {busy_master(c):.1f} %, {cyc:.0f} | {wb[0]:.0f}-{wb[1]:.0f} % | "
              f"{c['first_fail_mpps']:g} ({c['first_fail_limit']}, RX loss {c['first_fail_rx_loss_pct']} %) | {mu} |")
