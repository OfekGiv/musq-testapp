#!/usr/bin/env python3
"""Markdown tables of the improved distributor: per core count, the forwarded
rate of the best distributor (this campaign's sat.csv, mode best) against the
single master of finding 18 (dist), mu_sq and plain RSS (all from
../../musq-vs-swdist-work-2026-10-27/sat/sat.csv), with busy % and cycles per
packet and the master's own share.  best_table.py [sat.csv] [reference sat.csv]"""
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
    if r['mode'] in ('musq', 'plain', 'dist') and (int(r['cores']), r['work']) in by and r['mode'] not in by[(int(r['cores']), r['work'])]:
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
    print(f'\n**{cores} cores, 128 B** (best distributor and distributor: 1 master + {cores - 1} workers; mu_sq {cores} '
          f'lanes; plain {cores} RSS cores). Forwarded Mpps under overload (80 Mpps offered); busy % and cycles per '
          f'packet of the cores; "self" = packets the master processed itself.\n')
    print('| work/pkt | best distributor: Mpps; master busy, cyc; self Mpps; workers busy, cyc | '
          'distributor (finding 18): Mpps; master busy, cyc; workers busy, cyc | mu_sq: Mpps, lanes busy, cyc | plain: Mpps | '
          'best vs dist | best vs mu_sq | best vs plain |')
    print('|---|---|---|---|---|---|---|---|')
    for w in works:
        m = by[(cores, w)]
        be, di, mu, pl = m.get('best'), m.get('dist'), m.get('musq'), m.get('plain')
        print(f'| {w} | {f(be, "fwd_mpps", 2)}; {f(be, "master_busy", 0)} %, {f(be, "master_cyc", 0)}; '
              f'{f(be, "self_mpps", 2)}; '
              f'{f(be, "lanes_busy_min", 0)}-{f(be, "lanes_busy_max", 0)} %, {f(be, "lanes_cyc_min", 0)}-{f(be, "lanes_cyc_max", 0)} '
              f'| {f(di, "fwd_mpps", 2)}; {f(di, "master_busy", 0)} %, {f(di, "master_cyc", 0)}; '
              f'{f(di, "lanes_busy_min", 0)}-{f(di, "lanes_busy_max", 0)} %, {f(di, "lanes_cyc_min", 0)}-{f(di, "lanes_cyc_max", 0)} '
              f'| {f(mu, "fwd_mpps", 2)}, {f(mu, "lanes_busy_min", 0)}-{f(mu, "lanes_busy_max", 0)} %, '
              f'{f(mu, "lanes_cyc_min", 0)}-{f(mu, "lanes_cyc_max", 0)} | {f(pl, "fwd_mpps", 2)} '
              f'| {lead(be, di)} | {lead(be, mu)} | {lead(be, pl)} |')
