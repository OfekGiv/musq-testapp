#!/usr/bin/env python3
"""Markdown table from the latency runs (rtt_runs.py): one row per (service,
rate, cores), the three layouts side by side, plus optional extra variants
given as label=path/to/rtt.csv (e.g. the mu_sq default-doorbell runs).
  rtt_table.py rtt/rtt.csv [label=other/rtt.csv ...]"""
import csv
import sys
from collections import defaultdict

main = sys.argv[1]
extras = [a.split('=', 1) for a in sys.argv[2:]]
rows = defaultdict(dict)  # (service, rate, cores) -> label -> row
for r in csv.DictReader(open(main)):
    rows[(int(r['service_ns']), float(r['rate_mpps']), int(r['cores']))][r['mode']] = r
for label, path in extras:
    for r in csv.DictReader(open(path)):
        rows[(int(r['service_ns']), float(r['rate_mpps']), int(r['cores']))][label] = r
labels = ['musq', 'dist', 'plain'] + [l for l, _ in extras]
names = {'musq': 'mu_sq', 'dist': 'distributor', 'plain': 'plain RSS'}


def cell(r):
    if r is None:
        return '-'
    x = ' / '.join(f'{float(r[k]):.0f}' for k in ('p50', 'p99', 'p99.9'))
    loss = float(r['loss_pct'])
    extra = f', loss {loss:.0f} %' if loss > 0.5 else ''
    return f'{x} (cpu {float(r["cpu_avg"]):.0f} %{extra})'


print('| service | rate | cores | ' + ' | '.join(f'{names.get(l, l)}: p50 / p99 / p99.9 us' for l in labels) + ' |')
print('|---|---|---|' + '---|' * len(labels))
for key in sorted(rows):
    s, rate, cores = key
    print(f'| {s / 1000:g} us | {rate:g} Mpps | {cores} | ' + ' | '.join(cell(rows[key].get(l)) for l in labels) + ' |')
