#!/usr/bin/env python3
"""Max offered rate the responder forwards without loss, per (cores, frame):
the software distributor (RESP_MODE=dist, default of one_run.sh: 1 master +
cores - 1 workers), mu_sq (RESP_MODE=musq) or plain RSS (RESP_MODE=plain),
with the per-packet work in $WORK (one_run.sh). Case files and tags carry the
mode and the work. Pass = TX refused <= 0.01 % and RX loss (offered -
delivered, i.e. RX missed for the distributor) <= 0.01 %.

Per case: start at --start (per core count, else 80 % of the expected wall),
step up 2 Mpps while passing (down 4 if the start fails), bisect to the
resolution, then require 3 passing runs at the reported rate (step down one
resolution step otherwise). A fresh responder every run (one_run.sh).
Restartable: finished cases are in cases/.
  [SWEEP_OUT=<dir>] [TRAFFIC=udp] sweep.py [--cores 2,4,8] [--frames 98,128,256,512,1024,1518] [--start 2:20,4:26,8:26]
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_run import parse  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.environ.get('SWEEP_OUT', HERE))  # cases/, runs/, runs.csv, sweep.log go here
RUNS = os.path.join(OUT, 'runs')
CASES = os.path.join(OUT, 'cases')
RUNS_CSV = os.path.join(OUT, 'runs.csv')
LOG = os.path.join(OUT, 'sweep.log')
MUSQ_WALL = 36.0                       # 1 pkt/WQE, finding 15
PORT_WALL = {98: 1e9, 128: 1e9, 256: 55.7, 512: 38.2, 1024: 23.2, 1518: 16.4}  # testpmd, 2 queues
MODE = os.environ.get('RESP_MODE', 'dist')
WORK = os.environ.get('WORK', 'fixed:0')
CASE = f'{MODE}_w{WORK.replace(":", "")}'   # prefix of case files and run tags
RUN_COLS = ['time', 'mode', 'work', 'cores', 'frame', 'target_mpps', 'tag', 'phase', 'pass', 'limit', 'offered_mpps', 'rx_mpps',
            'tx_mpps', 'tx_refused_pct', 'rx_loss_pct', 'nic_discards', 'rx_missed', 'wire_bytes', 'rx_gbps_wire',
            'cpu_avg', 'cpu_max', 'cpu_peak_1s', 'cyc_per_pkt_avg', 'rx_share_spread_pct', 'windows', 'busy']


def log(msg):
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')


class Aborted(Exception):
    pass


seq = [0]


def run(cores, frame, mpps, phase):
    for attempt in range(2):
        seq[0] += 1
        tag = f'{CASE}_c{cores}_f{frame}_r{mpps:g}_{int(time.time())}_{seq[0]}'
        p = subprocess.run([os.path.join(HERE, 'one_run.sh'), str(cores), str(frame), f'{mpps:g}', tag],
                           capture_output=True, text=True, env=dict(os.environ, OUT=RUNS))
        if p.returncode == 2:
            raise Aborted('another musq-testapp is running: ' + p.stderr.strip())
        if p.returncode == 0:
            r = parse(os.path.join(RUNS, f'resp_{tag}.csv'), mpps)
            if r.get('ok') and r['windows'] >= 2:
                break
            log(f'  {tag}: unusable stats ({r.get("error", r.get("windows"))}), retrying')
        else:
            log(f'  {tag}: run failed rc={p.returncode}: {p.stderr.strip()[-300:]}, retrying')
        time.sleep(5)
    else:
        raise Aborted(f'two failed runs at c{cores} f{frame} {mpps:g} Mpps')
    row = {k: r.get(k) for k in RUN_COLS if k in r}
    row.update(time=time.strftime('%F %T'), mode=MODE, work=WORK, cores=cores, frame=frame, target_mpps=mpps,
               tag=tag, phase=phase,
               busy=json.dumps({k: round(v, 2) for k, v in r['busy'].items()}))
    new = not os.path.exists(RUNS_CSV)
    with open(RUNS_CSV, 'a', newline='') as f:
        w = csv.DictWriter(f, RUN_COLS)
        if new:
            w.writeheader()
        w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()})
    log(f'  c{cores} f{frame} {mpps:6.2f} Mpps {phase:8s} -> {"PASS" if r["pass"] else "FAIL " + r["limit"]:9s} '
        f'off {r["offered_mpps"]:.2f} tx {r["tx_mpps"]:.2f} refused {r["tx_refused_pct"]:.3f}% '
        f'rxloss {r["rx_loss_pct"]:.3f}% cpu avg {r["cpu_avg"]:.1f} max {r["cpu_max"]:.1f}')
    time.sleep(2)
    return r


START = {}  # cores -> start Mpps (--start)


def search(cores, frame):
    start = round(0.8 * min(START.get(cores, MUSQ_WALL), PORT_WALL.get(frame, 1e9)))  # small frames: no port wall
    res = 0.5 if start >= 20 else 0.25
    results = {}

    def at(rate, phase):
        r = run(cores, frame, rate, phase)
        results.setdefault(rate, []).append(r)
        return r['pass']

    lo = hi = None
    rate = float(start)
    if at(rate, 'ladder'):
        lo = rate
        while hi is None:
            rate += 2
            if rate > 90:
                raise Aborted('no TX overflow below 90 Mpps')
            if at(rate, 'ladder'):
                lo = rate
            else:
                hi = rate
    else:
        hi = rate
        while lo is None:
            rate -= 4
            if rate <= 0:
                raise Aborted('fails at every rate')
            if at(rate, 'ladder'):
                lo = rate
            else:
                hi = rate
    while hi - lo > res + 1e-9:
        mid = round(((lo + hi) / 2) / res) * res
        if mid <= lo or mid >= hi:
            break
        if at(mid, 'bisect'):
            lo = mid
        else:
            hi = mid
    while True:  # 3 passing runs at the reported rate
        while len(results.get(lo, [])) < 3 and all(r['pass'] for r in results.get(lo, [])):
            at(lo, 'confirm')
        if all(r['pass'] for r in results[lo]):
            break
        hi = lo
        lo = round(lo - res, 4)
        if lo <= 0:
            raise Aborted('confirm stepped down to 0')
    ok = results[lo]
    fail_runs = [r for r in results.get(hi, []) if not r['pass']]
    mean = lambda k, rs: sum(r[k] for r in rs) / len(rs)  # noqa: E731
    case = {
        'mode': MODE, 'work': WORK,
        'cores': cores, 'frame': frame, 'wire_bytes': round(mean('wire_bytes', ok), 1),
        'max_mpps': lo, 'forwarded_mpps': round(mean('tx_mpps', ok), 3), 'gbps_wire': round(mean('rx_gbps_wire', ok), 2),
        'cpu_avg': round(mean('cpu_avg', ok), 2), 'cpu_max': round(mean('cpu_max', ok), 2),
        'cpu_peak_1s': round(max(r['cpu_peak_1s'] for r in ok), 2), 'cyc_per_pkt': round(mean('cyc_per_pkt_avg', ok), 1),
        'busy_per_lcore': {k: round(sum(r['busy'][k] for r in ok) / len(ok), 2) for k in ok[0]['busy']},
        'first_fail_mpps': hi, 'first_fail_limit': fail_runs[0]['limit'] if fail_runs else '',
        'first_fail_tx_refused_pct': round(fail_runs[0]['tx_refused_pct'], 3) if fail_runs else None,
        'first_fail_rx_loss_pct': round(fail_runs[0]['rx_loss_pct'], 3) if fail_runs else None,
        'first_fail_cpu_max': round(fail_runs[0]['cpu_max'], 2) if fail_runs else None,
        'runs': sum(len(v) for v in results.values()), 'resolution': res,
    }
    return case


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cores', default='2,4,8')
    ap.add_argument('--frames', default='98,128,256,512,1024,1518')
    ap.add_argument('--start', default='2:26,4:26,8:26', help='start rate per core count (Mpps); the saturation runs put the master at about 31.6 Mpps for 256 B')
    a = ap.parse_args()
    for item in filter(None, a.start.split(',')):
        c, v = item.split(':')
        START[int(c)] = float(v)
    os.makedirs(RUNS, exist_ok=True)
    os.makedirs(CASES, exist_ok=True)
    log(f'output {OUT}, TRAFFIC={os.environ.get("TRAFFIC", "esp")}')
    freq = subprocess.run("for c in 0 2 4 6 8 10 12 14 16; do echo -n $(( $(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq)/1000 ))' '; done",
                          shell=True, capture_output=True, text=True).stdout
    log(f'sweep start, RESP_MODE={os.environ.get("RESP_MODE", "dist")} RESP_EXTRA={os.environ.get("RESP_EXTRA", "")!r}, '
        f'danger51 MHz cpu0,2..16: {freq}')
    for cores in map(int, a.cores.split(',')):
        for frame in map(int, a.frames.split(',')):
            path = os.path.join(CASES, f'{CASE}_c{cores}_f{frame}.json')
            if os.path.exists(path):
                continue
            log(f'case {MODE} {WORK}: {cores} cores, {frame} B')
            try:
                case = search(cores, frame)
            except Aborted as e:
                log(f'ABORT: {e}')
                sys.exit(1)
            with open(path, 'w') as f:
                json.dump(case, f, indent=1)
            log(f'case done: {MODE} {WORK} {cores} cores {frame} B: max {case["max_mpps"]} Mpps ({case["gbps_wire"]} Gbps), '
                f'cpu avg {case["cpu_avg"]} max {case["cpu_max"]}; next {case["first_fail_mpps"]} fails '
                f'{case["first_fail_limit"]} ({case["runs"]} runs)')
    log('sweep done')


if __name__ == '__main__':
    main()
