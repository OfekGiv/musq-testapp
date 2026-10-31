#!/usr/bin/env python3
"""Requester-side numbers of one run from its log: the last "RTT since
measurement start" table (All column, us) and the last per-core counter rows
summed over the lcores.  rtt.py <req_log> -> JSON"""
import json
import re
import sys

ROWS = ['Samples', 'Min us', 'Mean us', 'p50 us', 'p90 us', 'p99 us', 'p99.9 us', 'p99.99 us', 'Max us']
COUNTERS = ['Req sent', 'Resp matched', 'Resp unmatched', 'Req lost', 'Non-response RX']


def parse_req(path):
    lines = open(path, errors='replace').read().splitlines()
    starts = [i for i, l in enumerate(lines) if l.startswith('=== RTT since measurement start')]
    out = {'ok': bool(starts)}
    if starts:
        for l in lines[starts[-1]:starts[-1] + 16]:
            cells = [c.strip() for c in l.split('|')]
            if cells and cells[0] in ROWS and len(cells) > 1:
                v = cells[1]
                out[cells[0].replace(' us', '').lower()] = None if v == '-' else float(v.replace(',', ''))
    for name in COUNTERS:
        last = [l for l in lines if l.startswith(name + ' ')]
        if last:
            cells = [c.strip() for c in last[-1].split('|')][1:]
            out[name.lower().replace(' ', '_').replace('-', '_')] = sum(
                int(re.match(r'[\d,]+', c).group().replace(',', '')) for c in cells if re.match(r'[\d,]+', c))
    sent, matched = out.get('req_sent'), out.get('resp_matched')
    if sent:
        out['matched_pct'] = 100.0 * (matched or 0) / sent
    return out


if __name__ == '__main__':
    print(json.dumps(parse_req(sys.argv[1]), indent=1))
