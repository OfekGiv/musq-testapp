#!/usr/bin/env python3
"""Master tuning at 0 work: one saturation run per named variant of the
single-master distributor (DIST_OPTS / RESP_EXTRA / MUSQ_BIN per variant),
forwarded Mpps and the master's cycles per packet.
  tune.py [--cores 8] [--frame 128] [--offer 80] [--dur 7] [--variants a,b,...]
Output in $TUNE_OUT (default ../tune): runs/, tune.csv, tune.log."""
import argparse
import csv
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_run import parse  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
OUT = os.path.abspath(os.environ.get('TUNE_OUT', os.path.join(HERE, '..', 'tune')))
RUNS = os.path.join(OUT, 'runs')
CSV = os.path.join(OUT, 'tune.csv')
LOG = os.path.join(OUT, 'tune.log')
BASE = '--rx-burst 64 --tx-burst 64 --dist-chunk 32'
STOCK = os.path.join(REPO, 'build-nodoca-stock', 'musq-testapp')
VARIANTS = {
    'base':        dict(DIST_OPTS=BASE),
    'burst128':    dict(DIST_OPTS='--rx-burst 128 --tx-burst 128 --dist-chunk 64'),
    'rx128tx64':   dict(DIST_OPTS='--rx-burst 128 --tx-burst 64 --dist-chunk 32'),
    'chunk64':     dict(DIST_OPTS='--rx-burst 64 --tx-burst 64 --dist-chunk 64'),
    'chunk16':     dict(DIST_OPTS='--rx-burst 64 --tx-burst 64 --dist-chunk 16'),
    'chunk8':      dict(DIST_OPTS='--rx-burst 64 --tx-burst 64 --dist-chunk 8'),
    'immediate':   dict(DIST_OPTS=BASE, RESP_EXTRA='--tx-mode immediate'),
    'rxq2':        dict(DIST_OPTS=BASE, RESP_EXTRA='--rx-queues 2'),
    'rings2048':   dict(DIST_OPTS=BASE, RESP_EXTRA='--rxd 2048 --txd 2048'),
    'rxd4096':     dict(DIST_OPTS=BASE, RESP_EXTRA='--rxd 4096'),
    'seqn':        dict(DIST_OPTS=BASE + ' --dist-order seqn'),
    'self32':      dict(DIST_OPTS=BASE + ' --dist-self 32'),
    'stock':       dict(DIST_OPTS=BASE, MUSQ_BIN=STOCK),
    'stock_rxq2':  dict(DIST_OPTS=BASE, MUSQ_BIN=STOCK, RESP_EXTRA='--rx-queues 2'),
}
COLS = ['time', 'variant', 'cores', 'frame', 'offered_mpps', 'fwd_mpps', 'loss_pct', 'tx_refused_pct', 'master_busy',
        'master_cyc', 'workers_busy_max', 'workers_cyc_max', 'tag']


def log(msg):
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cores', type=int, default=8)
    ap.add_argument('--frame', type=int, default=128)
    ap.add_argument('--offer', type=float, default=80.0)
    ap.add_argument('--dur', type=int, default=7)
    ap.add_argument('--work', default='fixed:0')
    ap.add_argument('--variants', default=','.join(VARIANTS))
    a = ap.parse_args()
    os.makedirs(RUNS, exist_ok=True)
    log(f'tune: cores {a.cores} frame {a.frame} offer {a.offer} work {a.work} variants {a.variants}')
    for name in a.variants.split(','):
        # bin:<path> = the base settings on another binary (A/B of builds)
        v = dict(DIST_OPTS=BASE, MUSQ_BIN=name[4:]) if name.startswith('bin:') else VARIANTS[name]
        if name.startswith('bin:'):
            name = 'bin_' + os.path.basename(os.path.dirname(name[4:]))
        if 'MUSQ_BIN' in v and not os.path.exists(v['MUSQ_BIN']):
            log(f'  {name}: no binary {v["MUSQ_BIN"]}, skipped')
            continue
        tag = f'tune_{name}_c{a.cores}_w{a.work.replace(":", "")}_{int(time.time())}'
        env = dict(os.environ, OUT=RUNS, RESP_MODE='dist', WORK=a.work, TRAFFIC='esp', **v)
        p = subprocess.run([os.path.join(HERE, 'one_run.sh'), str(a.cores), str(a.frame), f'{a.offer:g}', tag, str(a.dur)],
                           capture_output=True, text=True, env=env)
        if p.returncode == 2:
            log('ABORT: another musq-testapp is running: ' + p.stderr.strip())
            sys.exit(1)
        if p.returncode != 0:
            log(f'  {name}: run failed rc={p.returncode}: {p.stderr.strip()[-300:]}')
            continue
        r = parse(os.path.join(RUNS, f'resp_{tag}.csv'), a.offer)
        if not r.get('ok'):
            log(f'  {name}: unusable stats {r}')
            continue
        b, c = r['busy'], r['cyc_per_pkt']
        workers = [l for l in b if l != 'lcore2']
        off, tx = r['offered_mpps'], r['tx_mpps']
        row = dict(time=time.strftime('%F %T'), variant=name, cores=a.cores, frame=a.frame, offered_mpps=off, fwd_mpps=tx,
                   loss_pct=100 * (off - tx) / off if off else None, tx_refused_pct=r['tx_refused_pct'],
                   master_busy=b['lcore2'], master_cyc=c['lcore2'], workers_busy_max=max(b[l] for l in workers),
                   workers_cyc_max=max(c[l] for l in workers), tag=tag)
        new = not os.path.exists(CSV)
        with open(CSV, 'a', newline='') as f:
            w = csv.DictWriter(f, COLS)
            if new:
                w.writeheader()
            w.writerow({k: (round(v, 3) if isinstance(v, float) else v) for k, v in row.items()})
        log(f'  {name:12s}: fwd {tx:.2f} Mpps (offered {off:.1f}) refused {r["tx_refused_pct"]:.3f} % | master '
            f'{b["lcore2"]:.0f} % {c["lcore2"]:.0f} cyc | workers busy max {row["workers_busy_max"]:.0f} % cyc max '
            f'{row["workers_cyc_max"]:.0f}')
        time.sleep(2)
    log('tune done')


if __name__ == '__main__':
    main()
