#!/usr/bin/env python3
"""Strict (finding-16 rule) confirmation of chosen saturation points: for each
mode:cores:work[:frame] the sweep.py search starts 1.5 Mpps below the
saturation matrix's forwarded rate (sat.csv; frame 128 by default).
  strict_points.py mode:cores:work[:frame] ...
Output in $SWEEP_OUT (default ../strict)."""
import csv
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SAT = os.environ.get('SAT_CSV', os.path.join(HERE, '..', 'sat', 'sat.csv'))
OUT = os.path.abspath(os.environ.get('SWEEP_OUT', os.path.join(HERE, '..', 'strict')))
TRAFFIC = {'musq': 'esp', 'dist': 'esp', 'best': 'esp', 'bestw': 'esp', 'split': 'esp', 'plain': 'udp'}
sat = {(r['mode'], int(r['cores']), r['work'], int(r['frame'])): float(r['fwd_mpps']) for r in csv.DictReader(open(SAT))}
for pt in sys.argv[1:]:
    parts = pt.split(':')
    mode, cores, work = parts[0], int(parts[1]), ':'.join(parts[2:4])
    frame = int(parts[4]) if len(parts) > 4 else 128
    fwd = sat[(mode, cores, work, frame)]
    start = (fwd - 1.5) / 0.8          # sweep.py starts at round(0.8 x START)
    env = dict(os.environ, SWEEP_OUT=OUT, RESP_MODE=mode, WORK=work,
               TRAFFIC=os.environ.get(f'TRAFFIC_{mode}', TRAFFIC[mode]))
    print(f'== {mode} {cores} cores {work} {frame} B: saturation {fwd:.2f} Mpps, search from {round(0.8 * start)}', flush=True)
    subprocess.run([sys.executable, os.path.join(HERE, 'sweep.py'), '--cores', str(cores), '--frames', str(frame),
                    '--start', f'{cores}:{start:.3f}'], env=env)
