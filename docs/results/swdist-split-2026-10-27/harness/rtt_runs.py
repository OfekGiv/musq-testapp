#!/usr/bin/env python3
"""Latency runs: one run per (mode, cores) at a fixed offered rate, responder
with --work header and --tx-mode immediate, requester with readable ESP
(TRAFFIC=esp_sw) and --service-time <ns>, so that it matches the responses
and reports the RTT percentiles.
  rtt_runs.py [--modes musq,dist,plain] [--cores 2,4,8] [--rate 16] [--service 0]
              [--frame 128] [--dur 7]
Traffic per mode: $TRAFFIC_<mode>, else esp_sw for musq and dist, udp for
plain (RSS on ESP hashes the IP pair only, 8 flows for 8 requester lcores,
and leaves queues uneven; UDP varies the source port).
Output in $RTT_OUT (default ../rtt): runs/, rtt.csv, rtt.log."""
import argparse
import csv
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_run import parse  # noqa: E402
from rtt import parse_req  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.environ.get('RTT_OUT', os.path.join(HERE, '..', 'rtt')))
RUNS = os.path.join(OUT, 'runs')
CSV = os.path.join(OUT, 'rtt.csv')
LOG = os.path.join(OUT, 'rtt.log')
COLS = ['time', 'mode', 'cores', 'service_ns', 'rate_mpps', 'frame', 'traffic', 'offered_mpps', 'fwd_mpps',
        'loss_pct', 'tx_refused_pct', 'matched_pct', 'samples', 'min', 'mean', 'p50', 'p90', 'p99', 'p99.9',
        'p99.99', 'max', 'cpu_avg', 'cpu_max', 'rx_share_spread_pct', 'tag']


def log(msg):
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--modes', default='split,dist')
    ap.add_argument('--cores', default='2,4,8')
    ap.add_argument('--rate', type=float, default=16.0, help='offered total Mpps')
    ap.add_argument('--service', type=int, default=0, help='service time per request, ns (sent as whole us)')
    ap.add_argument('--frame', type=int, default=128)
    ap.add_argument('--dur', type=int, default=7)
    a = ap.parse_args()
    os.makedirs(RUNS, exist_ok=True)
    log(f'rtt runs: modes {a.modes} cores {a.cores} rate {a.rate} Mpps service {a.service} ns frame {a.frame} '
        f'out {OUT}')
    for cores in map(int, a.cores.split(',')):
        for mode in a.modes.split(','):
            traffic = os.environ.get(f'TRAFFIC_{mode}', 'udp' if mode == 'plain' else 'esp_sw')
            tag = f'{mode}_c{cores}_s{a.service}_r{a.rate:g}_f{a.frame}_{int(time.time())}'
            env = dict(os.environ, OUT=RUNS, RESP_MODE=mode, WORK='header', TRAFFIC=traffic,
                       RESP_EXTRA=(os.environ.get('RESP_EXTRA', '') + ' --tx-mode immediate').strip(),
                       REQ_EXTRA=(os.environ.get('REQ_EXTRA', '') + f' --service-time fixed:{a.service}').strip())
            p = subprocess.run([os.path.join(HERE, 'one_run.sh'), str(cores), str(a.frame), f'{a.rate:g}', tag,
                                str(a.dur)], capture_output=True, text=True, env=env)
            if p.returncode == 2:
                log('ABORT: another musq-testapp is running: ' + p.stderr.strip())
                sys.exit(1)
            if p.returncode != 0:
                log(f'  {tag}: run failed rc={p.returncode}: {p.stderr.strip()[-300:]}')
                continue
            r = parse(os.path.join(RUNS, f'resp_{tag}.csv'), a.rate)
            q = parse_req(os.path.join(RUNS, f'req_{tag}.log'))
            off, tx = r['offered_mpps'], r['tx_mpps']
            row = dict(time=time.strftime('%F %T'), mode=mode, cores=cores, service_ns=a.service, rate_mpps=a.rate,
                       frame=a.frame, traffic=traffic, offered_mpps=off, fwd_mpps=tx,
                       loss_pct=100 * (off - tx) / off if off else None, tx_refused_pct=r['tx_refused_pct'],
                       matched_pct=q.get('matched_pct'), samples=q.get('samples'), cpu_avg=r['cpu_avg'],
                       cpu_max=r['cpu_max'], rx_share_spread_pct=r['rx_share_spread_pct'], tag=tag)
            for k in ('min', 'mean', 'p50', 'p90', 'p99', 'p99.9', 'p99.99', 'max'):
                row[k] = q.get(k)
            new = not os.path.exists(CSV)
            with open(CSV, 'a', newline='') as f:
                w = csv.DictWriter(f, COLS)
                if new:
                    w.writeheader()
                w.writerow({k: (round(v, 3) if isinstance(v, float) else v) for k, v in row.items()})
            fmt = lambda v: '-' if v is None else f'{v:.1f}'  # noqa: E731
            log(f'  {mode:5s} c{cores}: offered {off:.2f} fwd {tx:.2f} Mpps loss {row["loss_pct"]:.2f} % '
                f'matched {fmt(q.get("matched_pct"))} % | RTT us p50 {fmt(q.get("p50"))} p90 {fmt(q.get("p90"))} '
                f'p99 {fmt(q.get("p99"))} p99.9 {fmt(q.get("p99.9"))} max {fmt(q.get("max"))} | '
                f'cpu avg {r["cpu_avg"]:.0f} max {r["cpu_max"]:.0f} %')
            time.sleep(2)
    log('rtt runs done')


if __name__ == '__main__':
    main()
