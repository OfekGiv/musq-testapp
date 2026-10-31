#!/usr/bin/env python3
"""runs_lcores.csv (one row per run per lane core, steady window) from the
responder stats CSVs listed in runs.csv.  export.py <out_dir>"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_run import parse  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[1]
cols = ['tag', 'cores', 'frame', 'target_mpps', 'phase', 'pass', 'limit', 'lcore', 'busy_pct', 'cyc_per_pkt',
        'rx_pkts', 'tx_refused', 'window_s']
with open(os.path.join(HERE, 'runs.csv')) as f, open(os.path.join(out, 'runs_lcores.csv'), 'w', newline='') as g:
    w = csv.DictWriter(g, cols)
    w.writeheader()
    for run in csv.DictReader(f):
        r = parse(os.path.join(HERE, 'runs', f'resp_{run["tag"]}.csv'), float(run['target_mpps']))
        for lc in r['busy']:
            w.writerow({'tag': run['tag'], 'cores': run['cores'], 'frame': run['frame'],
                        'target_mpps': run['target_mpps'], 'phase': run['phase'], 'pass': run['pass'],
                        'limit': run['limit'], 'lcore': lc[5:], 'busy_pct': round(r['busy'][lc], 3),
                        'cyc_per_pkt': round(r['cyc_per_pkt'][lc], 2), 'rx_pkts': r['rx_per_lcore'][lc],
                        'tx_refused': r['refused_per_lcore'][lc], 'window_s': r['secs']})
