#!/usr/bin/env python3
"""Live CPU busy % per forwarding lcore of a running DPDK app, from its telemetry
endpoint /eal/lcore/usage (busy and total TSC cycles per lcore).

testpmd reports it only with --record-core-cycles; musq-testapp always does. A
polling lcore is always 100 % in top; this is the share of its cycles spent on
iterations that handled packets.

  sudo scripts/dpdk_lcore_usage.py                  # testpmd, default file prefix 'rte'
  sudo scripts/dpdk_lcore_usage.py --prefix musq    # musq-testapp
"""
import argparse
import json
import socket
import time


def query(sock, cmd):
    sock.send(cmd.encode())
    return json.loads(sock.recv(1 << 20).decode())[cmd]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--prefix', default='rte', help='EAL --file-prefix of the app (testpmd default: rte)')
    ap.add_argument('--interval', type=float, default=1.0, help='seconds between reports')
    args = ap.parse_args()

    path = f'/var/run/dpdk/{args.prefix}/dpdk_telemetry.v2'
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.connect(path)
    sock.recv(1024)  # greeting with the app's pid and version

    prev = None
    while True:
        u = query(sock, '/eal/lcore/usage')
        if not u or not u.get('lcore_ids'):
            print('no lcore usage reported (testpmd needs --record-core-cycles)')
            return
        cur = {lc: (b, t) for lc, b, t in zip(u['lcore_ids'], u['busy_cycles'], u['total_cycles'])}
        if prev:
            cells = []
            for lc, (b, t) in cur.items():
                pb, pt = prev.get(lc, (0, 0))
                win = 100.0 * (b - pb) / (t - pt) if t > pt else 0.0
                avg = 100.0 * b / t if t else 0.0
                cells.append(f'lcore {lc}: {win:5.1f} % busy (avg {avg:5.1f} %)')
            print(time.strftime('%H:%M:%S'), ' | '.join(cells), flush=True)
        prev = cur
        time.sleep(args.interval)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (FileNotFoundError, ConnectionRefusedError):
        print('no telemetry socket: is the app running, and is --prefix its --file-prefix?')
