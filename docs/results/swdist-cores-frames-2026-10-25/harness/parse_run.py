#!/usr/bin/env python3
"""Steady-state numbers of one responder run from its --stats-csv.

Plateau = 1 s windows whose offered rate (rx_phy_packets) is >= 98 % of the
highest window's, first and last of them dropped. Pass = TX refused <= 0.01 %
of received, RX loss (offered - delivered) <= 0.01 %, offered >= 98 % of target.
"""
import csv
import json
import sys
from collections import defaultdict

TX_TOL = 1e-4
RX_TOL = 1e-4
GEN_TOL = 0.98


def parse(path, target_mpps=None):
    win = defaultdict(lambda: defaultdict(dict))  # window -> (scope,id) -> metric -> (total, delta, rate)
    win_s = {}
    with open(path) as f:
        for row in csv.reader(f):
            if len(row) != 11 or row[4] != '0':   # skip the header and the final (since-start) report
                continue
            w = int(row[1])
            win_s[w] = float(row[3])
            win[w][(row[5], row[6])][row[7]] = (int(row[8]), int(row[9]), float(row[10]))
    ws = sorted(win)
    offered = {w: win[w][('summary', 'port0')].get('offered_pps', (0, 0, 0.0))[2] for w in ws}
    top = max(offered.values(), default=0.0)
    if top <= 0:
        return {'ok': False, 'error': 'no traffic'}
    plat = [w for w in ws if offered[w] >= 0.98 * top]
    if len(plat) >= 4:
        plat = plat[1:-1]
    lcores = sorted({i for w in ws for (s, i) in win[w] if s == 'core'}, key=lambda s: int(s[5:]))

    def dsum(scope, ident, metric):
        return sum(win[w][(scope, ident)].get(metric, (0, 0, 0))[1] for w in plat)

    secs = sum(win_s[w] for w in plat)
    off = dsum('summary', 'port0', 'offered_pps')
    rx = dsum('summary', 'port0', 'rx_pps')
    tx = dsum('summary', 'port0', 'tx_pps')
    disc = dsum('summary', 'port0', 'nic_discards')
    missed = dsum('port', 'port0', 'rx_missed')
    rxb = dsum('port', 'port0', 'rx_bytes')
    refused = sum(dsum('core', c, 'tx_drops') for c in lcores)
    busy, cpp, rxs, refd, peak = {}, {}, {}, {}, 0.0
    for c in lcores:
        u = dsum('core', c, 'cycles_used')
        a = u + dsum('core', c, 'cycles_wasted')
        n = dsum('core', c, 'rx_packets')
        busy[c] = 100.0 * u / a if a else 0.0
        cpp[c] = u / n if n else 0.0
        rxs[c] = n
        refd[c] = dsum('core', c, 'tx_drops')
        for w in plat:
            m = win[w][('core', c)]
            uw, aw = m['cycles_used'][1], m['cycles_used'][1] + m['cycles_wasted'][1]
            if aw:
                peak = max(peak, 100.0 * uw / aw)
    tx_frac = refused / rx if rx else 1.0
    rx_frac = max(0, off - rx) / off if off else 1.0
    gen_ok = target_mpps is None or off / secs >= GEN_TOL * target_mpps * 1e6
    fails = []
    if tx_frac > TX_TOL:
        fails.append('TX')
    if rx_frac > RX_TOL:
        fails.append('RX')
    if not gen_ok:
        fails.append('GEN')
    bv = list(busy.values())
    return {
        'ok': True, 'pass': not fails, 'limit': '+'.join(fails), 'windows': len(plat), 'secs': round(secs, 3),
        'target_mpps': target_mpps, 'offered_mpps': off / secs / 1e6, 'rx_mpps': rx / secs / 1e6,
        'tx_mpps': tx / secs / 1e6, 'tx_refused_pct': 100 * tx_frac, 'rx_loss_pct': 100 * rx_frac,
        'nic_discards': disc, 'rx_missed': missed,
        'wire_bytes': rxb / rx + 4 if rx else 0.0, 'rx_gbps_wire': (rxb + 4 * rx) * 8 / secs / 1e9,
        'cpu_avg': sum(bv) / len(bv), 'cpu_max': max(bv), 'cpu_peak_1s': peak,
        'cyc_per_pkt_avg': sum(cpp.values()) / len(cpp),
        'rx_share_spread_pct': 100 * (max(rxs.values()) - min(rxs.values())) / max(1, max(rxs.values())),
        'busy': busy, 'cyc_per_pkt': cpp, 'rx_per_lcore': rxs, 'refused_per_lcore': refd,
    }


if __name__ == '__main__':
    r = parse(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else None)
    print(json.dumps(r, indent=1, default=lambda x: round(x, 3)))
