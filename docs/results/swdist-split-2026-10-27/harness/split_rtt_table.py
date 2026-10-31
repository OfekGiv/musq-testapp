#!/usr/bin/env python3
"""Latency table of the split-pipeline test: this campaign's split and
single-master rows (rtt/rtt.csv) beside the 2026-10-27 work matrix's mu_sq
(PMD doorbell rule), distributor and plain RSS rows, by (service, rate, cores).
  split_rtt_table.py [rtt.csv] [reference dir]"""
import csv
import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'rtt', 'rtt.csv')
ref = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', '..', 'musq-vs-swdist-work-2026-10-27')
rows = defaultdict(dict)  # (service, rate, cores) -> label -> row
for r in csv.DictReader(open(path)):
    rows[(int(r['service_ns']), float(r['rate_mpps']), int(r['cores']))][r['mode']] = r
keys = set(rows)
for sub, modes, suffix in (('rtt', ('dist', 'plain'), '_ref'), ('rtt-musq-dbdefault', ('musq',), '_ref')):
    for r in csv.DictReader(open(os.path.join(ref, sub, 'rtt.csv'))):
        k = (int(r['service_ns']), float(r['rate_mpps']), int(r['cores']))
        if r['mode'] in modes and k in keys:
            rows[k][r['mode'] + suffix] = r
labels = [('split', 'split'), ('dist', 'distributor (today)'), ('dist_ref', 'distributor (finding 18)'),
          ('musq_ref', 'mu_sq, PMD doorbell (finding 18)'), ('plain_ref', 'plain RSS (finding 18)')]


def cell(r):
    if r is None:
        return '-'
    x = ' / '.join(f'{float(r[k]):.0f}' for k in ('p50', 'p99', 'p99.9'))
    loss = float(r['loss_pct'])
    extra = f', loss {loss:.0f} %' if loss > 0.5 else ''
    return f'{x} (cpu {float(r["cpu_avg"]):.0f} %{extra})'


print('| service | rate | cores | ' + ' | '.join(f'{n}: p50 / p99 / p99.9 us' for _, n in labels) + ' |')
print('|---|---|---|' + '---|' * len(labels))
for key in sorted(rows):
    s, rate, cores = key
    print(f'| {s / 1000:g} us | {rate:g} Mpps | {cores} | ' + ' | '.join(cell(rows[key].get(l)) for l, _ in labels) + ' |')
