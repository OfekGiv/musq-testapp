#!/usr/bin/env python3
"""Strict (finding-16 rule) confirmation of chosen saturation points: for each
mode:cores:work the sweep.py search starts 1.5 Mpps below the saturation
matrix's forwarded rate (sat.csv).  strict_points.py mode:cores:work ...
Output in $SWEEP_OUT (default ../strict)."""
import csv
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SAT = os.environ.get('SAT_CSV', os.path.join(HERE, '..', 'sat', 'sat.csv'))
OUT = os.path.abspath(os.environ.get('SWEEP_OUT', os.path.join(HERE, '..', 'strict')))
TRAFFIC = {'musq': 'esp', 'dist': 'esp', 'plain': 'udp'}
sat = {(r['mode'], int(r['cores']), r['work']): float(r['fwd_mpps']) for r in csv.DictReader(open(SAT))}
for pt in sys.argv[1:]:
    mode, cores, work = pt.split(':', 2)
    cores = int(cores)
    fwd = sat[(mode, cores, work)]
    start = (fwd - 1.5) / 0.8          # sweep.py starts at round(0.8 x START)
    env = dict(os.environ, SWEEP_OUT=OUT, RESP_MODE=mode, WORK=work,
               TRAFFIC=os.environ.get(f'TRAFFIC_{mode}', TRAFFIC[mode]))
    print(f'== {mode} {cores} cores {work}: saturation {fwd:.2f} Mpps, search from {round(0.8 * start)}', flush=True)
    subprocess.run([sys.executable, os.path.join(HERE, 'sweep.py'), '--cores', str(cores), '--frames', '128',
                    '--start', f'{cores}:{start:.3f}'], env=env)
