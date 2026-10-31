#!/usr/bin/env python3
"""What limits one queue at 128 B: RX queue vs plain SQ vs mu_sq, offered-rate ladders.

Every run: a fresh DUT on danger51 (this host), a paced requester on danger50
(8 lcores, 7 s of traffic), NIC phy counters read on both hosts before and after
the run (end-to-end packet accounting) and at two points inside the steady part.
Refuses to start next to any musq-testapp or testpmd it did not start.

  qrun.py [--cases a,b] [--rates 40,44] [--rep N]     # finished points are skipped
"""
import argparse
import csv
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time
from collections import defaultdict

REPO = '/homes/ofekdg/musq-testapp'
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RAW = os.path.join(ROOT, 'raw')
RUNS = os.path.join(ROOT, 'runs.csv')
REQ = 'danger50'
IFACE = 'enp23s0f1np1'
TSC_MHZ = 2394
TESTPMD = '/homes/ofekdg/dpdk-eval/dpdk-v25.11/build/app/dpdk-testpmd'
COUNTERS = ('rx_packets_phy', 'tx_packets_phy', 'rx_discards_phy', 'rx_prio0_buf_discard', 'rx_out_of_buffer')
APP = 'MUSQ_BUILD=o3 scripts/run.sh --stats-interval 1 --stats-csv {csv} '
ONE_CORE = ('--lcores 20 --main-lcore 18 --doca-mode none --musq-group 0 --rx-burst 64 --tx-burst 64 '
            '--tx-mode immediate --swap none ')
UDP = '--traffic udp --frame-size 128 --sport 1025-2048'
ESP = '--traffic esp --esp-encap hw --frame-size 128'
CASES = {
    # The user's testpmd command, single core and queue, stock DPDK 25.11.
    'testpmd_1rx': dict(
        label='testpmd: 1 RX queue, 1 TX queue', kind='testpmd', workers=1, traffic=UDP,
        rates=[30, 32, 34, 35, 36, 37, 38, 40, 44, 48, 60],
        cmd=f'sudo -n stdbuf -oL {TESTPMD} -l 18,20 -n 4 -a 0000:17:00.1 --huge-dir /mnt/huge-1G --socket-mem 2048,0 '
            '--file-prefix tpmd_dut -- --socket-num=0 --burst=64 --txd=1024 --rxd=1024 --mbcache=512 --rxq=1 --txq=1 '
            '--nb-cores=1 --total-num-mbufs=180224 -a --record-core-cycles --record-burst-stats --stats-period 1'),
    # Run 2: the same queue layout in musq-testapp, paced.
    'app_1rx_empw': dict(
        label='musq-testapp: 1 RX queue, 1 TX queue, multi-packet WQEs', kind='app', workers=1, traffic=UDP,
        rates=[30, 32, 34, 35, 36, 37, 38, 40, 44, 48, 60],
        cmd=APP + ONE_CORE + '--txd 1024 --rxd 1024 --rx-queues 1'),
    # Reference: one plain SQ with multi-packet WQEs and RX spread over 2 queues.
    'app_2rx_empw': dict(
        label='musq-testapp: 2 RX queues, 1 TX queue, multi-packet WQEs', kind='app', workers=1, traffic=UDP,
        rates=[36, 40, 44, 48, 52, 56, 60, 64],
        cmd=APP + ONE_CORE + '--txd 2048 --rxd 1024 --rx-queues 2'),
    # Run 1: one plain SQ doing one WQE per packet.
    'app_2rx_1wqe': dict(
        label='musq-testapp: 2 RX queues, 1 TX queue, 1 packet per WQE', kind='app', workers=1, traffic=UDP,
        rates=[36, 38, 40, 42, 44, 46, 48, 52, 56, 60],
        cmd=APP + ONE_CORE + '--txd 2048 --rxd 1024 --rx-queues 2 --pkts-per-wqe 1'),
    # Run 3: mu_sq as in the sweep, 2 lanes on one ring, sweep's loop flags.
    'musq2': dict(
        label='mu_sq: 2 cores, 2 RX queues, shared ring, 1 packet per WQE', kind='app', workers=2, traffic=ESP,
        rates=[36, 38, 40, 41, 42, 43, 44, 46, 48, 52, 56],
        cmd=APP + '--lcores 20,22 --main-lcore 18 --doca-mode esp_rr --pkts-per-wqe 1 --txd 2048 --rxd 1024 '
                  '--devargs mu_sq_comp_n=128 --db-wqes 128'),
}
COLS = ['time', 'case', 'target_mpps', 'rep', 'tag', 'pass', 'limit', 'offered_mpps', 'delivered_mpps', 'forwarded_mpps',
        'tx_phy_mpps', 'tx_refused_pct', 'rx_lost_pct', 'nic_discard_mpps', 'rx_missed_mpps', 'cpu_avg', 'cpu_max',
        'cyc_per_pkt', 'avg_rx_burst', 'windows', 'd50_tx_total', 'd51_rx_total', 'd51_tx_total', 'd50_rx_total',
        'link_loss_to_dut', 'link_loss_to_req', 'steady_d51_tx_mpps', 'steady_d50_rx_mpps', 'steady_d51_discard_mpps']


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)


def eth(host=None):
    pat = '|'.join(COUNTERS)
    # Timestamps inside the sudo call: sudo's own delay varies, ethtool's does not.
    inner = f'date +%s.%N; ethtool -S {IFACE} | grep -E "^ +({pat}):"; date +%s.%N'
    if host is None:
        r = subprocess.run(['sudo', '-n', 'sh', '-c', inner], capture_output=True, text=True)
    else:
        r = subprocess.run(['ssh', '-o', 'BatchMode=yes', host, f"sudo -n sh -c '{inner}'"], capture_output=True, text=True)
    lines = r.stdout.split('\n')
    t0, out = float(lines[0]), {}
    for ln in lines[1:]:
        m = re.match(r'\s*(\w+):\s*(\d+)', ln)
        if m:
            out[m[1]] = int(m[2])
        elif ln.strip():
            out['t'] = (t0 + float(ln)) / 2
    return out


def eth_both():
    res = {}
    th = [threading.Thread(target=lambda h=h: res.__setitem__(h or 'dut', eth(h))) for h in (None, REQ)]
    [t.start() for t in th]
    [t.join() for t in th]
    return res['dut'], res[REQ]


def busy_hosts():
    pat = 'pgrep -a -x "musq-testapp|dpdk-testpmd"'
    return (sh(pat).stdout + sh(f"ssh -o BatchMode=yes {REQ} '{pat}'").stdout).strip()


def child_pid(root):
    for name in ('musq-testapp', 'dpdk-testpmd'):
        for p in sh(f'pgrep -x {name}').stdout.split():
            q = int(p)
            while q > 1:
                if q == root:
                    return int(p)
                try:
                    q = int(re.search(r'PPid:\s*(\d+)', open(f'/proc/{q}/status').read())[1])
                except (OSError, TypeError):
                    break
    return None


def stop(proc):
    for sig in ('INT', 'INT', 'KILL'):
        pid = child_pid(proc.pid)
        if pid:
            sh(f'sudo -n kill -s {sig} {pid}')
        else:
            try:
                proc.send_signal(2)
            except ProcessLookupError:
                pass
        try:
            proc.wait(timeout=30)
            return
        except subprocess.TimeoutExpired:
            pass


def parse_app(csv_path, target):
    """Same plateau rule as the sweep's parse_run.py, plus the wire counter."""
    win = defaultdict(lambda: defaultdict(dict))
    secs = {}
    with open(csv_path) as f:
        for x in csv.reader(f):
            if len(x) != 11 or x[4] != '0':
                continue
            w = int(x[1])
            secs[w] = float(x[3])
            win[w][(x[5], x[6])][x[7]] = int(x[9])
    off = {w: win[w][('summary', 'port0')].get('offered_pps', 0) / secs[w] for w in win}
    top = max(off.values())
    plat = [w for w in sorted(win) if off[w] >= 0.98 * top]
    plat = plat[1:-1] if len(plat) >= 4 else plat
    T = sum(secs[w] for w in plat)
    get = lambda s, i, m: sum(win[w][(s, i)].get(m, 0) for w in plat)  # noqa: E731
    lcores = sorted({i for w in win for s, i in win[w] if s == 'core'})
    offered, rx, tx = (get('summary', 'port0', k) for k in ('offered_pps', 'rx_pps', 'tx_pps'))
    refused = sum(get('core', c, 'tx_drops') for c in lcores)
    busy, cyc = [], []
    pkts = bursts = 0
    for c in lcores:
        u, a, n = get('core', c, 'cycles_used'), get('core', c, 'cycles_wasted'), get('core', c, 'rx_packets')
        busy.append(100 * u / (u + a) if u + a else 0)
        cyc.append(u / n if n else 0)
        pkts += n
        bursts += get('core', c, 'rx_bursts')
    return dict(offered_mpps=offered / T / 1e6, delivered_mpps=rx / T / 1e6, forwarded_mpps=tx / T / 1e6,
                tx_phy_mpps=get('xstat', 'port0', 'tx_phy_packets') / T / 1e6, refused=refused / rx if rx else 1,
                lost=max(0, offered - rx) / offered if offered else 1,
                nic_discard_mpps=get('summary', 'port0', 'nic_discards') / T / 1e6,
                rx_missed_mpps=get('port', 'port0', 'rx_missed') / T / 1e6, cpu_avg=sum(busy) / len(busy),
                cpu_max=max(busy), cyc_per_pkt=sum(cyc) / len(cyc), avg_rx_burst=pkts / bursts if bursts else 0,
                windows=len(plat))


def parse_testpmd(log, a, b):
    """testpmd's per-second Rx-pps/Tx-pps (TSC timed) and its exit line; refusal and
    loss from its forward stats, offered from the NIC counters read in the steady part."""
    text = open(log, errors='replace').read()
    rx = [int(v) for v in re.findall(r'Rx-pps:\s+(\d+)', text)]
    tx = [int(v) for v in re.findall(r'Tx-pps:\s+(\d+)', text)]
    top = max(rx)
    idx = [i for i, v in enumerate(rx) if v >= 0.9 * top][1:-1]
    rx_m, tx_m = statistics.mean(rx[i] for i in idx) / 1e6, statistics.mean(tx[i] for i in idx) / 1e6
    dt = b['t'] - a['t']
    offered = (b['rx_packets_phy'] - a['rx_packets_phy']) / dt / 1e6
    disc = (b['rx_prio0_buf_discard'] - a['rx_prio0_buf_discard']) / dt / 1e6
    disc += (b['rx_discards_phy'] - a['rx_discards_phy']) / dt / 1e6
    m = re.search(r'CPU cycles/packet=([\d.]+)', text)
    cyc = float(m[1]) if m else 0
    dropped = re.search(r'Accumulated forward statistics.*?TX-packets:\s*\d+\s+TX-dropped:\s*(\d+)', text, re.S)
    rxp = re.search(r'Accumulated forward statistics.*?RX-packets:\s*(\d+)', text, re.S)
    refused = int(dropped[1]) / int(rxp[1]) if dropped and rxp and int(rxp[1]) else 0
    # Both counters from one ethtool read at each end of the steady window, so their
    # ratio is exact whatever the timing; what the wire did not send back and TX did
    # not refuse was lost on RX.
    d_rx, d_tx = b['rx_packets_phy'] - a['rx_packets_phy'], b['tx_packets_phy'] - a['tx_packets_phy']
    lost = max(0.0, 1 - d_tx / d_rx - refused) if d_rx else 1.0
    burst = re.search(r'RX-bursts\s*:\s*(\d+)\s*\[(\d+)% of 0 pkts', text)
    nonempty = int(burst[1]) * (100 - int(burst[2])) / 100 if burst else 0
    busy = cyc * rx_m / TSC_MHZ * 100
    return dict(offered_mpps=offered, delivered_mpps=rx_m, forwarded_mpps=tx_m,
                tx_phy_mpps=(b['tx_packets_phy'] - a['tx_packets_phy']) / dt / 1e6, refused=refused,
                lost=lost, nic_discard_mpps=disc, rx_missed_mpps=0,
                cpu_avg=busy, cpu_max=busy, cyc_per_pkt=cyc,
                avg_rx_burst=int(rxp[1]) / nonempty if rxp and nonempty else 0, windows=len(idx))


def log(msg):
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True)
    with open(os.path.join(ROOT, 'qrun.log'), 'a') as f:
        f.write(line + '\n')


def run_one(name, target, rep):
    c = CASES[name]
    tag = f'{name}_r{target:g}_{rep}'
    dlog, qlog, ccsv = (os.path.join(RAW, f'{p}_{tag}.{e}') for p, e in (('dut', 'log'), ('req', 'log'), ('dut', 'csv')))
    sh(f'sudo -n rm -f {ccsv}')
    who = busy_hosts()
    if who:
        raise RuntimeError('another instance is running, not starting: ' + who)
    with open(dlog, 'w') as f:
        dut = subprocess.Popen(c['cmd'].format(csv=ccsv), shell=True, cwd=REPO, stdin=subprocess.DEVNULL, stdout=f,
                               stderr=subprocess.STDOUT)
    ready = 'forwards packets on' if c['kind'] == 'testpmd' else 'entering main loop'
    t = time.time()
    while open(dlog, errors='replace').read().count(ready) < c['workers']:
        if dut.poll() is not None or time.time() - t > 60:
            stop(dut)
            raise RuntimeError(f'{tag}: DUT not ready, see {dlog}')
        time.sleep(0.1)
    time.sleep(1)
    s0_dut, s0_req = eth_both()
    rps = round(target * 1e6 / 8)
    with open(qlog, 'w') as f:
        req = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', REQ,
                                f"cd {REPO} && scripts/run.sh --role requester --lcores 2,4,6,8,10,12,14,16 "
                                f"--doca-mode none --musq-group 0 {c['traffic']} --rps {rps} --warmup 1 --duration 7"],
                               stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
    t = time.time()
    while eth()['rx_packets_phy'] - s0_dut['rx_packets_phy'] < 1000000:
        if req.poll() is not None or time.time() - t > 60:
            stop(dut)
            raise RuntimeError(f'{tag}: no traffic, see {qlog}')
        time.sleep(0.5)
    time.sleep(2)
    a_dut, a_req = eth_both()
    time.sleep(3)
    b_dut, b_req = eth_both()
    req_rc = req.wait()
    time.sleep(1.5)
    s1_dut, s1_req = eth_both()
    stop(dut)
    if req_rc:
        raise RuntimeError(f'{tag}: requester exited {req_rc}, see {qlog}')
    r = parse_testpmd(dlog, a_dut, b_dut) if c['kind'] == 'testpmd' else parse_app(ccsv, target)
    fails = []
    if r['refused'] > 1e-4:
        fails.append('TX')
    if r['lost'] > 1e-4:
        fails.append('RX')
    if r['offered_mpps'] < 0.98 * target:
        fails.append('GEN')
    d = lambda s0, s1, k: s1[k] - s0[k]  # noqa: E731
    dt_d, dt_q = b_dut['t'] - a_dut['t'], b_req['t'] - a_req['t']
    row = dict(time=time.strftime('%F %T'), case=name, target_mpps=target, rep=rep, tag=tag, windows=r['windows'],
               limit='+'.join(fails), tx_refused_pct=100 * r['refused'], rx_lost_pct=100 * r['lost'],
               d50_tx_total=d(s0_req, s1_req, 'tx_packets_phy'), d51_rx_total=d(s0_dut, s1_dut, 'rx_packets_phy'),
               d51_tx_total=d(s0_dut, s1_dut, 'tx_packets_phy'), d50_rx_total=d(s0_req, s1_req, 'rx_packets_phy'),
               steady_d51_tx_mpps=d(a_dut, b_dut, 'tx_packets_phy') / dt_d / 1e6,
               steady_d50_rx_mpps=d(a_req, b_req, 'rx_packets_phy') / dt_q / 1e6,
               steady_d51_discard_mpps=(d(a_dut, b_dut, 'rx_prio0_buf_discard') + d(a_dut, b_dut, 'rx_discards_phy')) / dt_d / 1e6,
               **{k: r[k] for k in ('offered_mpps', 'delivered_mpps', 'forwarded_mpps', 'tx_phy_mpps', 'nic_discard_mpps',
                                    'rx_missed_mpps', 'cpu_avg', 'cpu_max', 'cyc_per_pkt', 'avg_rx_burst')})
    row['pass'] = not fails
    row['link_loss_to_dut'] = row['d50_tx_total'] - row['d51_rx_total']
    row['link_loss_to_req'] = row['d51_tx_total'] - row['d50_rx_total']
    new = not os.path.exists(RUNS)
    with open(RUNS, 'a', newline='') as f:
        w = csv.DictWriter(f, COLS)
        if new:
            w.writeheader()
        w.writerow({k: round(v, 4) if isinstance(v, float) else v for k, v in row.items()})
    log(f"{name:13s} {target:5.1f} -> {'PASS' if not fails else 'FAIL ' + row['limit']:12s} off {r['offered_mpps']:6.2f} "
        f"deliv {r['delivered_mpps']:6.2f} fwd {r['forwarded_mpps']:6.2f} wire {r['tx_phy_mpps']:6.2f} "
        f"refused {100 * r['refused']:7.3f}% rxlost {100 * r['lost']:7.3f}% cpu {r['cpu_avg']:5.1f} cyc {r['cyc_per_pkt']:5.0f} "
        f"burst {r['avg_rx_burst']:4.1f} | d50rx-d51tx {row['d50_rx_total'] - row['d51_tx_total']:+d}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cases', default=','.join(CASES))
    ap.add_argument('--rates', default='')
    ap.add_argument('--rep', type=int, default=1)
    a = ap.parse_args()
    os.makedirs(RAW, exist_ok=True)
    done = set()
    if os.path.exists(RUNS):
        with open(RUNS) as f:
            done = {(r['case'], float(r['target_mpps']), int(r['rep'])) for r in csv.DictReader(f)}
    for name in a.cases.split(','):
        rates = [float(x) for x in a.rates.split(',')] if a.rates else CASES[name]['rates']
        for target in rates:
            if (name, float(target), a.rep) in done:
                continue
            try:
                run_one(name, target, a.rep)
            except RuntimeError as e:
                log(f'ABORT: {e}')
                sys.exit(1)
            time.sleep(2)
    log('done ' + json.dumps({'cases': a.cases, 'rep': a.rep}))


if __name__ == '__main__':
    main()
