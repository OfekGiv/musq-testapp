#!/usr/bin/env python3
"""Markdown tables from sat.csv (sat.py): per core count, the forwarded rate of
each layout against the per-packet work, mu_sq's lead, and the cost in
core-cycles per packet (cores x 2.4 GHz / forwarded rate: what the whole
layout spends per packet, work included).  sat_table.py [sat.csv]"""
import csv
import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'sat', 'sat.csv')
TSC = 2.4e9
rows = list(csv.DictReader(open(path)))
by = defaultdict(dict)   # (frame, cores, work) -> mode -> row
for r in rows:
    by[(int(r['frame']), int(r['cores']), r['work'])][r['mode']] = r


def work_ns(w):
    return int(w.split(':')[1]) if ':' in w else -1


def f(r, k, d=1):
    return '-' if r is None or r.get(k) in (None, '') else f'{float(r[k]):.{d}f}'


def lead(a, b):
    if a is None or b is None:
        return '-'
    x, y = float(a['fwd_mpps']), float(b['fwd_mpps'])
    return f'{100 * (x / y - 1):+.0f} %' if y else '-'


def core_cyc(r, cores):
    return '-' if r is None or not float(r['fwd_mpps']) else f'{cores * TSC / (float(r["fwd_mpps"]) * 1e6):.0f}'


for (frame, cores) in sorted({(k[0], k[1]) for k in by}):
    works = sorted({k[2] for k in by if k[0] == frame and k[1] == cores}, key=work_ns)
    print(f'\n**{cores} cores, {frame} B** (mu_sq {cores} lanes, distributor 1 master + {cores - 1} workers, '
          f'plain {cores} RSS cores). Forwarded Mpps under overload; busy % and cycles per packet of the cores.\n')
    print('| work/pkt | mu_sq: Mpps, lanes busy, cyc | distributor: Mpps, master busy, cyc; workers busy, cyc | '
          'plain: Mpps, busy, cyc, RX spread | mu_sq vs dist | mu_sq vs plain | core-cyc/pkt mu_sq / dist / plain |')
    print('|---|---|---|---|---|---|---|')
    for w in works:
        m = by[(frame, cores, w)]
        mu, di, pl = m.get('musq'), m.get('dist'), m.get('plain')
        print(f'| {w} | {f(mu, "fwd_mpps", 2)}, {f(mu, "lanes_busy_min", 0)}-{f(mu, "lanes_busy_max", 0)} %, '
              f'{f(mu, "lanes_cyc_min", 0)}-{f(mu, "lanes_cyc_max", 0)} '
              f'| {f(di, "fwd_mpps", 2)}, {f(di, "master_busy", 0)} %, {f(di, "master_cyc", 0)}; '
              f'{f(di, "lanes_busy_min", 0)}-{f(di, "lanes_busy_max", 0)} %, {f(di, "lanes_cyc_min", 0)}-{f(di, "lanes_cyc_max", 0)} '
              f'| {f(pl, "fwd_mpps", 2)}, {f(pl, "lanes_busy_min", 0)}-{f(pl, "lanes_busy_max", 0)} %, '
              f'{f(pl, "lanes_cyc_min", 0)}-{f(pl, "lanes_cyc_max", 0)}, {f(pl, "rx_share_spread_pct", 0)} % '
              f'| {lead(mu, di)} | {lead(mu, pl)} | {core_cyc(mu, cores)} / {core_cyc(di, cores)} / {core_cyc(pl, cores)} |')
