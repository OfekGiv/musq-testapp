#!/usr/bin/env python3
"""Print per-lane TX WQ status from the mlx5 /mlx5/txq/lane_wq telemetry.

Each TX queue's own core records its WQ state inside rte_eth_tx_burst (see
struct mlx5_txq_lane_tel in the prom-dpdk mlx5 PMD); this script only reads
those lane-owned records over the DPDK telemetry socket, so polling it does not
touch the lanes' hot cache lines. Needs root (the socket belongs to the app).

    sudo scripts/musq_lane_wq.py                 # every 1 s until Ctrl-C
    sudo scripts/musq_lane_wq.py -i 0.5 -n 20    # 20 intervals of 0.5 s
    sudo scripts/musq_lane_wq.py --raw           # one raw JSON snapshot

Per interval and lane:
  calls/s     rte_eth_tx_burst calls           refused%  calls that sent < asked
  pkts refused/s                               free avg  mean free WQEBBs at a
  no-room%    resource checks with 0 free      free min  resource check (min is
  last free/used  latest sample                          cumulative)
  no-slot%    checks with fewer free WQEBBs than a post needs (room_need: one
              lane stride + lap pad under mu_sq, e.g. 9*4+4 = 40 for 4 lanes)
"""
import argparse
import json
import socket
import sys
import time

CMD = b"/mlx5/txq/lane_wq"


def connect(prefix):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.connect(f"/var/run/dpdk/{prefix}/dpdk_telemetry.v2")
    hello = json.loads(s.recv(16384).decode())
    return s, hello.get("max_output_len", 16384)


def query(s, bufsz):
    s.send(CMD)
    reply = json.loads(s.recv(bufsz).decode())
    d = reply.get(CMD.decode())
    if d is None:
        sys.exit("the PMD has no /mlx5/txq/lane_wq command (built without MLX5_MU_LANE_TELEMETRY?)")
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--prefix", default="musq", help="EAL --file-prefix of the app (default musq)")
    ap.add_argument("-i", "--interval", type=float, default=1.0, help="seconds between reports")
    ap.add_argument("-n", "--count", type=int, default=0, help="number of reports (0 = until Ctrl-C)")
    ap.add_argument("--raw", action="store_true", help="print one raw JSON snapshot and exit")
    a = ap.parse_args()

    s, bufsz = connect(a.prefix)
    prev = query(s, bufsz)
    if a.raw:
        print(json.dumps(prev, indent=1))
        return
    if not prev.get("queue"):
        sys.exit("no TX queue has transmitted yet")
    t_prev = time.monotonic()
    done = 0
    hdr = (f"{'port/q':>7} {'grp':>3} {'calls/s':>11} {'refused%':>8} {'pkts refused/s':>15} "
           f"{'free avg':>9} {'free min':>8} {'need':>5} {'no-room%':>8} {'no-slot%':>8} {'last free/used':>15}")
    try:
        while a.count == 0 or done < a.count:
            time.sleep(a.interval)
            cur = query(s, bufsz)
            t = time.monotonic()
            dt = t - t_prev
            print(f"\n--- {time.strftime('%H:%M:%S')}  window {dt:.3f}s  (ring {cur['wqe_s'][0]} WQEBBs,"
                  f" usable {cur['wqe_usable'][0]}) ---")
            print(hdr)
            for i in range(len(cur["queue"])):
                def dl(k):
                    return cur[k][i] - prev[k][i] if i < len(prev[k]) else cur[k][i]
                calls, refused, prefused = dl("calls"), dl("calls_refused"), dl("pkts_refused")
                passes, noroom, fsum = dl("passes"), dl("passes_no_room"), dl("free_sum")
                noslot = dl("passes_no_slot")
                print(f"{cur['port_id'][i]:>3}/{cur['queue'][i]:<3} {1 << cur['log_group_size'][i] if cur['log_group_size'][i] else 0:>3} "
                      f"{calls / dt:>11,.0f} {100.0 * refused / calls if calls else 0:>7.2f}% "
                      f"{prefused / dt:>15,.0f} {fsum / passes if passes else 0:>9.0f} {cur['free_min'][i]:>8} "
                      f"{cur['room_need'][i]:>5} {100.0 * noroom / passes if passes else 0:>7.2f}% "
                      f"{100.0 * noslot / passes if passes else 0:>7.2f}% "
                      f"{cur['last_free'][i]:>7}/{cur['last_used'][i]:<7}")
            prev, t_prev = cur, t
            done += 1
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
