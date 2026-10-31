#!/usr/bin/env python3
"""Saturation matrix: one run per (mode, cores, work) at an offered rate above
every ceiling; the forwarded rate (port TX pps over the steady window, where
the offered rate is >= 98 % of its peak) is the layout's capacity, the loss is
what the NIC dropped on RX (RX missed / discards) plus what TX refused.
  sat.py [--modes musq,dist,plain] [--cores 2,4,8] [--works fixed:0,fixed:100,...]
         [--frame 128] [--offer 64] [--dur 7]
Traffic per mode: $TRAFFIC_<mode>, else esp (NIC-encapsulated) for musq and
dist, udp for plain (RSS needs the varying UDP ports to spread 8 requester
lcores evenly). Output in $SAT_OUT (default ../sat): runs/, sat.csv, sat.log.
Restartable: (mode, cores, work, frame) rows already in sat.csv are skipped.
Per-core numbers: for dist the "master" columns are lcore 2 and the "lanes"
columns the workers; for split the master columns are the RX master (lcore 2),
the txm columns the TX master (lcore 4) and the lanes the workers; for musq
and plain the lanes are all cores.
"""
import argparse
import csv
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_run import parse  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.environ.get('SAT_OUT', os.path.join(HERE, '..', 'sat')))
RUNS = os.path.join(OUT, 'runs')
CSV = os.path.join(OUT, 'sat.csv')
LOG = os.path.join(OUT, 'sat.log')
COLS = ['time', 'mode', 'cores', 'work', 'frame', 'traffic', 'offered_mpps', 'rx_mpps', 'fwd_mpps', 'loss_pct',
        'tx_refused_pct', 'rx_missed', 'nic_discards', 'wire_bytes', 'master_busy', 'master_cyc',
        'txm_busy', 'txm_cyc', 'self_mpps', 'lanes_busy_min', 'lanes_busy_max', 'lanes_cyc_min', 'lanes_cyc_max', 'rx_share_spread_pct',
        'cpu_avg', 'windows', 'tag']
DEFAULT_TRAFFIC = {'musq': 'esp', 'dist': 'esp', 'best': 'esp', 'bestw': 'esp', 'split': 'esp', 'plain': 'udp'}


def log(msg):
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')


def summarize(r, mode):
    b, c = r['busy'], r['cyc_per_pkt']
    lanes = sorted(b, key=lambda s: int(s[5:]))
    row = {'master_busy': None, 'master_cyc': None, 'txm_busy': None, 'txm_cyc': None}
    if mode in ('dist', 'best', 'bestw', 'split'):
        row['master_busy'], row['master_cyc'] = b['lcore2'], c['lcore2']
        lanes = [l for l in lanes if l != 'lcore2']
    if mode == 'split':
        row['txm_busy'], row['txm_cyc'] = b['lcore4'], c['lcore4']
        lanes = [l for l in lanes if l != 'lcore4']
    row.update(lanes_busy_min=min(b[l] for l in lanes), lanes_busy_max=max(b[l] for l in lanes),
               lanes_cyc_min=min(c[l] for l in lanes), lanes_cyc_max=max(c[l] for l in lanes))
    return row


def one(mode, cores, work, frame, offer, dur, traffic):
    seq = int(time.time())
    for attempt in range(2):
        tag = f'{mode}_c{cores}_w{work.replace(":", "")}_f{frame}_{seq}_{attempt}'
        env = dict(os.environ, OUT=RUNS, RESP_MODE=mode, WORK=work, TRAFFIC=traffic)
        p = subprocess.run([os.path.join(HERE, 'one_run.sh'), str(cores), str(frame), f'{offer:g}', tag, str(dur)],
                           capture_output=True, text=True, env=env)
        if p.returncode == 2:
            log('ABORT: another musq-testapp is running: ' + p.stderr.strip())
            sys.exit(1)
        if p.returncode == 0:
            r = parse(os.path.join(RUNS, f'resp_{tag}.csv'), offer)
            if r.get('ok') and r['windows'] >= 2:
                break
            log(f'  {tag}: unusable stats ({r.get("error", r.get("windows"))}), retrying')
        else:
            log(f'  {tag}: run failed rc={p.returncode}: {p.stderr.strip()[-300:]}, retrying')
        time.sleep(5)
    else:
        log(f'ABORT: two failed runs for {mode} c{cores} {work}')
        sys.exit(1)
    off, tx = r['offered_mpps'], r['tx_mpps']
    row = dict(time=time.strftime('%F %T'), mode=mode, cores=cores, work=work, frame=frame, traffic=traffic,
               offered_mpps=off, rx_mpps=r['rx_mpps'], fwd_mpps=tx, loss_pct=100 * (off - tx) / off if off else None,
               tx_refused_pct=r['tx_refused_pct'], rx_missed=r['rx_missed'], nic_discards=r['nic_discards'],
               self_mpps=r['self_mpps'],
               wire_bytes=r['wire_bytes'], rx_share_spread_pct=r['rx_share_spread_pct'], cpu_avg=r['cpu_avg'],
               windows=r['windows'], tag=tag)
    row.update(summarize(r, mode))
    new = not os.path.exists(CSV)
    with open(CSV, 'a', newline='') as f:
        w = csv.DictWriter(f, COLS)
        if new:
            w.writeheader()
        w.writerow({k: (round(v, 3) if isinstance(v, float) else v) for k, v in row.items()})
    m = f'master {row["master_busy"]:.0f} % {row["master_cyc"]:.0f} cyc, ' if mode in ('dist', 'best', 'bestw', 'split') else ''
    if mode == 'split':
        m += f'tx master {row["txm_busy"]:.0f} % {row["txm_cyc"]:.0f} cyc, '
    if r['self_mpps']:
        m += f'self {r["self_mpps"]:.2f} Mpps, '
    log(f'  {mode:5s} c{cores} {work:10s} f{frame}: offered {off:.2f} fwd {tx:.2f} Mpps loss {row["loss_pct"]:.1f} % '
        f'refused {r["tx_refused_pct"]:.3f} % | {m}lanes busy {row["lanes_busy_min"]:.0f}-{row["lanes_busy_max"]:.0f} % '
        f'cyc {row["lanes_cyc_min"]:.0f}-{row["lanes_cyc_max"]:.0f}, rx spread {r["rx_share_spread_pct"]:.1f} %')
    time.sleep(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--modes', default='best')
    ap.add_argument('--cores', default='2,4,8')
    ap.add_argument('--works', default='fixed:0,fixed:50,fixed:100,fixed:200,fixed:400,fixed:800')
    ap.add_argument('--frame', type=int, default=128)
    ap.add_argument('--offer', type=float, default=64.0, help='offered total Mpps')
    ap.add_argument('--dur', type=int, default=7)
    a = ap.parse_args()
    os.makedirs(RUNS, exist_ok=True)
    done = set()
    if os.path.exists(CSV):
        for r in csv.DictReader(open(CSV)):
            done.add((r['mode'], int(r['cores']), r['work'], int(r['frame'])))
    log(f'matrix start: modes {a.modes} cores {a.cores} works {a.works} frame {a.frame} offer {a.offer} Mpps, out {OUT}')
    for work in a.works.split(','):
        for cores in map(int, a.cores.split(',')):
            for mode in a.modes.split(','):
                if (mode, cores, work, a.frame) in done:
                    continue
                traffic = os.environ.get(f'TRAFFIC_{mode}', DEFAULT_TRAFFIC[mode])
                one(mode, cores, work, a.frame, a.offer, a.dur, traffic)
    log('matrix done')


if __name__ == '__main__':
    main()
