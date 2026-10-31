#!/usr/bin/env python3
"""Markdown tables of the split-pipeline test: per core count, the forwarded
rate of the split distributor against the single-master distributor (both
from this campaign's sat.csv) and mu_sq and plain RSS (from the
2026-10-27 work matrix, ../../musq-vs-swdist-work-2026-10-27/sat/sat.csv),
with the busy % and cycles per packet of the masters and the workers.
  split_table.py [sat.csv] [reference sat.csv]"""
import csv
import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'sat', 'sat.csv')
ref = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', '..', 'musq-vs-swdist-work-2026-10-27', 'sat', 'sat.csv')
by = defaultdict(dict)   # (cores, work) -> mode -> row
for r in csv.DictReader(open(path)):
    by[(int(r['cores']), r['work'])][r['mode']] = r
for r in csv.DictReader(open(ref)):
    if r['mode'] in ('musq', 'plain') and (int(r['cores']), r['work']) in by:
        by[(int(r['cores']), r['work'])][r['mode']] = r


def work_key(w):
    kind, ns = w.split(':')
    return (kind != 'fixed', int(ns))


def f(r, k, d=1):
    return '-' if r is None or r.get(k) in (None, '') else f'{float(r[k]):.{d}f}'


def lead(a, b):
    if a is None or b is None:
        return '-'
    x, y = float(a['fwd_mpps']), float(b['fwd_mpps'])
    return f'{100 * (x / y - 1):+.0f} %' if y else '-'


for cores in sorted({k[0] for k in by}):
    works = sorted({k[1] for k in by if k[0] == cores}, key=work_key)
    print(f'\n**{cores} cores, 128 B** (split: RX master + TX master + {cores - 2} workers; distributor: 1 master + '
          f'{cores - 1} workers; mu_sq {cores} lanes; plain {cores} RSS cores). Forwarded Mpps under overload '
          f'(80 Mpps offered); busy % and cycles per packet of the cores.\n')
    print('| work/pkt | split: Mpps; RX master busy, cyc; TX master busy, cyc; workers busy, cyc | '
          'distributor: Mpps; master busy, cyc; workers busy, cyc | mu_sq: Mpps, lanes busy, cyc | plain: Mpps | '
          'split vs dist | split vs mu_sq | split vs plain |')
    print('|---|---|---|---|---|---|---|---|')
    for w in works:
        m = by[(cores, w)]
        sp, di, mu, pl = m.get('split'), m.get('dist'), m.get('musq'), m.get('plain')
        print(f'| {w} | {f(sp, "fwd_mpps", 2)}; {f(sp, "master_busy", 0)} %, {f(sp, "master_cyc", 0)}; '
              f'{f(sp, "txm_busy", 0)} %, {f(sp, "txm_cyc", 0)}; '
              f'{f(sp, "lanes_busy_min", 0)}-{f(sp, "lanes_busy_max", 0)} %, {f(sp, "lanes_cyc_min", 0)}-{f(sp, "lanes_cyc_max", 0)} '
              f'| {f(di, "fwd_mpps", 2)}; {f(di, "master_busy", 0)} %, {f(di, "master_cyc", 0)}; '
              f'{f(di, "lanes_busy_min", 0)}-{f(di, "lanes_busy_max", 0)} %, {f(di, "lanes_cyc_min", 0)}-{f(di, "lanes_cyc_max", 0)} '
              f'| {f(mu, "fwd_mpps", 2)}, {f(mu, "lanes_busy_min", 0)}-{f(mu, "lanes_busy_max", 0)} %, '
              f'{f(mu, "lanes_cyc_min", 0)}-{f(mu, "lanes_cyc_max", 0)} | {f(pl, "fwd_mpps", 2)} '
              f'| {lead(sp, di)} | {lead(sp, mu)} | {lead(sp, pl)} |')
