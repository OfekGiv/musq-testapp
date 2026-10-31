#!/usr/bin/env python3
"""Workbook from runs.csv: docs/results/queue-limits-128B-2026-10-31.xlsx.
Measured numbers are plain values (no LibreOffice on the hosts to recalculate
formulas); the charts are native Excel charts on the Table sheet's cells."""
import csv
import os
from collections import defaultdict

from openpyxl import Workbook
from openpyxl.chart import Reference, ScatterChart, Series
from openpyxl.chart.axis import ChartLines
from openpyxl.chart.marker import Marker
from openpyxl.chart.shapes import GraphicalProperties
from openpyxl.drawing.line import LineProperties
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter as L

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(os.path.dirname(ROOT), 'queue-limits-128B-2026-10-31.xlsx')
# case, short label for the charts, RX queues, TX, WQEs, marker, colour (palette slots 1-5, darker steps)
CASES = [
    ('testpmd_1rx', 'testpmd: 1 RX queue, 1 TX queue', 1, 'plain SQ', 'multi-packet', 'circle', '2A78D6'),
    ('app_1rx_empw', 'musq-testapp: 1 RX queue, 1 TX queue', 1, 'plain SQ', 'multi-packet', 'square', 'D95926'),
    ('app_2rx_empw', 'musq-testapp: 2 RX queues, 1 TX queue, multi-packet WQEs', 2, 'plain SQ', 'multi-packet', 'triangle', '199E70'),
    ('app_2rx_1wqe', 'musq-testapp: 2 RX queues, 1 TX queue, 1 packet per WQE', 2, 'plain SQ', '1 per packet', 'diamond', 'C98500'),
    ('musq2', 'mu_sq: 2 cores, 2 RX queues, shared ring, 1 packet per WQE', 2, 'shared ring, 2 lanes', '1 per packet', 'x', 'D55181'),
]
F = 'Arial'
HEAD = PatternFill('solid', fgColor='E8E8E4')
THIN = Side(style='thin', color='C3C2B7')
GRID = '#E1E0D9'.lstrip('#')
AXIS = '898781'


def load():
    with open(os.path.join(ROOT, 'runs.csv')) as f:
        return list(csv.DictReader(f))


def agg(rows):
    by = defaultdict(lambda: defaultdict(list))
    for r in rows:
        if int(r['rep']) < 90:
            by[r['case']][float(r['target_mpps'])].append(r)
    out = {}
    for case, pts in by.items():
        lst = []
        for t in sorted(pts):
            rs = pts[t]
            m = lambda k: sum(float(r[k]) for r in rs) / len(rs)  # noqa: E731
            lim = sorted({x for r in rs for x in r['limit'].split('+') if x})
            lst.append(dict(target=t, n=len(rs), offered=m('offered_mpps'), delivered=m('delivered_mpps'), fwd=m('forwarded_mpps'),
                            fmin=min(float(r['forwarded_mpps']) for r in rs), fmax=max(float(r['forwarded_mpps']) for r in rs),
                            wire=m('tx_phy_mpps'), refused=m('tx_refused_pct'), lost=m('rx_lost_pct'), cpu=m('cpu_avg'),
                            cpumax=max(float(r['cpu_max']) for r in rs), cyc=m('cyc_per_pkt'), burst=m('avg_rx_burst'),
                            passed=all(r['pass'] == 'True' for r in rs), npass=sum(r['pass'] == 'True' for r in rs), limit='+'.join(lim)))
        out[case] = lst
    return out


def cell(ws, r, c, v, bold=False, fmt=None, wrap=False, fill=None, size=10, align=None):
    x = ws.cell(row=r, column=c, value=v)
    x.font = Font(name=F, size=size, bold=bold)
    if fmt:
        x.number_format = fmt
    if wrap or align:
        x.alignment = Alignment(wrap_text=wrap, vertical='top', horizontal=align)
    if fill:
        x.fill = fill
    return x


def build_table(wb, data):
    ws = wb.create_sheet('Table')
    cell(ws, 1, 1, 'One row per offered rate: means over the runs at that rate (3 runs around each limit, otherwise 1)', bold=True, size=12)
    cols = ['Offered target Mpps', 'Runs', 'Offered at the port Mpps', 'Delivered to the RX queues Mpps', 'Forwarded Mpps', 'Forwarded min', 'Forwarded max',
            'On the wire (NIC counter) Mpps', 'TX refused %', 'RX lost %', 'CPU busy % (average over cores)', 'CPU busy %, busiest core', 'Busy cycles/pkt', 'Avg RX burst', 'Runs passing', 'What failed']
    fmts = ['0.0', '0', '0.00', '0.00', '0.00', '0.00', '0.00', '0.00', '0.000', '0.000', '0.0', '0.0', '0', '0.0', '0', None]
    keys = ['target', 'n', 'offered', 'delivered', 'fwd', 'fmin', 'fmax', 'wire', 'refused', 'lost', 'cpu', 'cpumax', 'cyc', 'burst', 'npass', 'limit']
    r = 3
    refs = {}
    for case, label, *_ in CASES:
        cell(ws, r, 1, label, bold=True)
        r += 1
        for c, h in enumerate(cols, 1):
            cell(ws, r, c, h, bold=True, wrap=True, fill=HEAD)
        ws.row_dimensions[r].height = 54
        r += 1
        first = r
        for p in data.get(case, []):
            for c, (k, f) in enumerate(zip(keys, fmts), 1):
                v = p[k]
                if k == 'limit' and not v:
                    v = '-'
                cell(ws, r, c, v, fmt=f)
            r += 1
        refs[case] = (first, r - 1)
        r += 1
    cell(ws, r, 1, 'Reference: forwarded = offered', bold=True)
    r += 1
    cell(ws, r, 1, 'Offered Mpps', bold=True, wrap=True, fill=HEAD)
    cell(ws, r, 3, 'Offered Mpps', bold=True, wrap=True, fill=HEAD)
    cell(ws, r, 5, 'Forwarded Mpps', bold=True, wrap=True, fill=HEAD)
    r += 1
    first = r
    for v in (28, 66):
        cell(ws, r, 3, v, fmt='0')
        cell(ws, r, 5, v, fmt='0')
        r += 1
    refs['_ref'] = (first, r - 1)
    for c in range(1, len(cols) + 1):
        ws.column_dimensions[L(c)].width = 13
    ws.freeze_panes = 'A3'
    return ws, refs


def style_chart(ch, title, ytitle, xmin, xmax, xunit, ymin, ymax, yunit):
    ch.title = title
    ch.style = 2
    ch.scatterStyle = 'lineMarker'
    ch.width, ch.height = 30, 16
    ch.legend.position = 'b'
    ch.x_axis.title = 'Offered at the port (Mpps), 128 B frames'
    ch.y_axis.title = ytitle
    for ax, lo, hi, unit in ((ch.x_axis, xmin, xmax, xunit), (ch.y_axis, ymin, ymax, yunit)):
        ax.scaling.min, ax.scaling.max = lo, hi
        ax.majorUnit = unit
        ax.delete = False
        ax.number_format = '0'
        ax.tickLblPos = 'low'
        ax.majorGridlines = ChartLines(spPr=GraphicalProperties(ln=LineProperties(solidFill=GRID, w=9525)))
        ax.graphicalProperties = GraphicalProperties(ln=LineProperties(solidFill=AXIS, w=9525))


def series(ws, col_y, first, last, label, marker, color, col_x=3):
    s = Series(Reference(ws, min_col=col_y, min_row=first, max_row=last),
               Reference(ws, min_col=col_x, min_row=first, max_row=last), title=label)
    s.smooth = False
    s.graphicalProperties = GraphicalProperties(ln=LineProperties(solidFill=color, w=22000))
    s.marker = Marker(symbol=marker, size=9)
    s.marker.graphicalProperties = GraphicalProperties(solidFill=color, ln=LineProperties(solidFill=color))
    return s


def build_graph(wb, ws, refs):
    g = wb.create_sheet('Graph', 1)
    c1 = ScatterChart()
    style_chart(c1, 'Forwarded against offered rate: what each queue layout holds', 'Forwarded (Mpps)', 28, 66, 4, 28, 66, 4)
    s = Series(Reference(ws, min_col=5, min_row=refs['_ref'][0], max_row=refs['_ref'][1]),
               Reference(ws, min_col=3, min_row=refs['_ref'][0], max_row=refs['_ref'][1]), title='forwarded = offered (nothing lost)')
    s.smooth = False
    s.graphicalProperties = GraphicalProperties(ln=LineProperties(solidFill='B5B4AD', w=12700, prstDash='dash'))
    s.marker = Marker(symbol='none')
    c1.series.append(s)
    for case, label, _, _, _, mk, col in CASES:
        c1.series.append(series(ws, 5, *refs[case], label, mk, col))
    g.add_chart(c1, 'A1')
    c2 = ScatterChart()
    style_chart(c2, 'CPU busy % of the forwarding cores at each offered rate', 'CPU busy %, average over the cores', 28, 66, 4, 60, 100, 5)
    for case, label, _, _, _, mk, col in CASES:
        c2.series.append(series(ws, 11, *refs[case], label, mk, col))
    g.add_chart(c2, 'A34')
    return g


def build_runs(wb, rows):
    ws = wb.create_sheet('Runs')
    cols = [c for c in rows[0].keys() if c != 'tag']
    for c, h in enumerate(cols, 1):
        cell(ws, 1, c, h, bold=True, wrap=True, fill=HEAD)
    for r, row in enumerate([x for x in rows if int(x['rep']) < 90], 2):
        for c, h in enumerate(cols, 1):
            v = row[h]
            try:
                v = float(v) if '.' in v else int(v)
            except ValueError:
                pass
            cell(ws, r, c, v)
    ws.row_dimensions[1].height = 42
    for c in range(1, len(cols) + 1):
        ws.column_dimensions[L(c)].width = 13
    ws.column_dimensions['B'].width = 15
    ws.column_dimensions['A'].width = 19
    ws.freeze_panes = 'A2'


def limits(data, case):
    pts = data[case]
    ok = 0.0
    for p in pts:   # highest rate whose runs all passed, with every lower rate passing too
        if not p['passed']:
            break
        ok = p['target']
    top = max(pts, key=lambda p: p['fwd'])
    first_fail = next((p for p in pts if not p['passed']), None)
    return ok, top, first_fail


def build_summary(wb, data, rows):
    ws = wb.active
    ws.title = 'Summary'
    cell(ws, 1, 1, 'What limits one queue at 128 B: RX queue, plain send queue, mu_sq (2026-10-31)', bold=True, size=13)
    t = {c[0]: limits(data, c[0]) for c in CASES}
    real = [r for r in rows if int(r['rep']) < 90]
    d_in = max(abs(int(r['link_loss_to_dut'])) for r in real)
    d_out = max(abs(int(r['link_loss_to_req'])) for r in real)
    wire = max(abs(float(r['tx_phy_mpps']) - float(r['forwarded_mpps'])) for r in real)
    tp, a1, a2, a3, mu = (t[k] for k in ('testpmd_1rx', 'app_1rx_empw', 'app_2rx_empw', 'app_2rx_1wqe', 'musq2'))
    at = lambda case, rate: next(p for p in data[case] if p['target'] == rate)  # noqa: E731
    text = [
        ('Question', 'Why does mu_sq forward about 44 Mpps when testpmd with one queue stops at about 37? Is the mu_sq test wrong?'),
        ('Answer', f"The mu_sq numbers hold up. They are limits of different queues: one RX queue stops at about 35 Mpps at 128 B, and one send queue at one packet per WQE stops at about 43 M WQEs per second. "
                   'mu_sq spreads RX over two queues, so it reaches the send-queue limit, which is the higher of the two.'),
        ('Result 1: RX queue', f"With one RX queue, paced with no overload, testpmd forwards everything up to {tp[0]:g} Mpps and musq-testapp up to {a1[0]:g}; above that both level off at about 35 Mpps (highest means {tp[1]['fwd']:.1f} and {a1[1]['fwd']:.1f}). "
                               f"The excess is lost on RX and TX refuses nothing (at 44 offered: {at('testpmd_1rx', 44)['lost']:.1f} % lost on RX, 0 refused). So this limit is real and not an effect of overload."),
        ('Result 2: send queue, multi-packet WQEs', f"One plain TX queue with RX spread over 2 queues forwards everything up to {a2[0]:g} Mpps, and {a2[1]['fwd']:.1f} at most with the core 100 % busy, TX refusing nothing. So the send queue is not what stopped the one-RX-queue runs."),
        ('Result 3: send queue, 1 packet per WQE', f"The same plain, unshared queue with one WQE per packet forwards everything up to {a3[0]:g} Mpps, then levels off at {a3[1]['fwd']:.1f} with TX refusing the rest ({at('app_2rx_1wqe', 48)['refused']:.1f} % at 48 offered) while RX still delivers everything. "
                                                   'The core is close to full too (97-100 %), but a core that runs out shows as RX loss with room left in the TX ring, not as TX refusals with RX complete, which is what appears from 42 to 48 offered; RX loss joins in only from 52. '
                                                   'The only change from Result 2 is one WQE per packet instead of many, so the limit follows the WQE rate, and no sharing is involved.'),
        ('Result 4: mu_sq', f"2 cores, 2 RX queues, one shared ring: everything forwarded up to {mu[0]:g} Mpps, levelling off at {mu[1]['fwd']:.1f} with TX refusing the rest. That is within {100 * (mu[1]['fwd'] / a3[1]['fwd'] - 1):.0f} % of the plain queue in Result 3, so sharing the ring costs little here, and mu_sq's ceiling at 128 B looks like the same one-WQE-per-packet limit."),
        ('Are the rates real?', f"Yes. In all {len(real)} runs the NIC port counters agree across the link: danger50 TX = danger51 RX (largest difference {d_in} packets) and danger51 TX = danger50 RX (largest difference {d_out} packets), over whole runs of {min(int(r['d51_tx_total']) for r in real) / 1e6:.0f} to {max(int(r['d51_rx_total']) for r in real) / 1e6:.0f} M packets. "
                                f"The wire counter is within {wire:.3f} Mpps of the driver's TX count."),
        ('Differences that remain', 'Plain-queue runs use UDP with RSS and SEND WQEs on one core with testpmd-like loop flags (burst 64, immediate, no header swap). mu_sq uses ESP with esp_rr, one-packet eMPW slots, a CQE and a doorbell every 128 slots, and the sweep\'s default loop flags. '
                                    'So a 3 % gap between Results 3 and 4 is not a measured advantage of either. How the NIC handles the WQE rate internally (WQE fetch, scheduling) is not visible from these runs.'),
        ('Conditions', 'danger51 DUT: Xeon Silver 4314 at 2.4 GHz, performance governor, numa_balancing 0, pause off, forwarding lcores 20 (and 22), main lcore 18, NIC async IRQ on CPU 4. danger50 requester: 8 lcores, paced, 7 s of traffic. Fresh DUT for every run. '
                       'Pass: TX refused and RX lost each at most 0.01 %, offered at least 98 % of the target. musq-testapp and mu_sq on prom-dpdk at -O3; testpmd is stock DPDK 25.11. Frames: 128 B on the wire for UDP, 126 B for NIC-encapsulated ESP.'),
        ('Sheets', 'Graph: forwarded against offered, and CPU busy against offered. Table: the numbers behind the charts. Runs: every run, including the port counters of both hosts.'),
        ('Documents', 'docs/musq-investigation.md finding 22; docs/results/queue-limits-2026-10-31 (README, runs.csv, per-run logs and stats, harness).'),
    ]
    r = 3
    for k, v in text:
        cell(ws, r, 1, k, bold=True, wrap=True)
        cell(ws, r, 2, v, wrap=True)
        ws.merge_cells(start_row=r, start_column=2, end_row=r, end_column=8)
        ws.row_dimensions[r].height = max(16, 14.5 * (len(v) // 125 + 1))
        r += 1
    r += 1
    heads = ['Layout', 'RX queues', 'TX', 'WQEs', 'Nothing lost up to (Mpps)', 'Most forwarded (Mpps)', 'First failure', 'CPU busy % at most forwarded']
    for c, h in enumerate(heads, 1):
        cell(ws, r, c, h, bold=True, wrap=True, fill=HEAD)
    ws.row_dimensions[r].height = 42
    r += 1
    for case, label, nrx, tx, wq, *_ in CASES:
        ok, top, fail = t[case]
        vals = [label, nrx, tx, wq, ok, round(top['fwd'], 1), (f"{fail['limit']} at {fail['target']:g} offered" if fail else '-'), round(top['cpu'], 1)]
        for c, v in enumerate(vals, 1):
            cell(ws, r, c, v, wrap=True)
        ws.row_dimensions[r].height = 42
        r += 1
    ws.column_dimensions['A'].width = 30
    for c, w in zip('BCDEFGH', (10, 14, 13, 14, 14, 18, 14)):
        ws.column_dimensions[c].width = w


def main():
    rows = load()
    data = agg(rows)
    wb = Workbook()
    build_summary(wb, data, rows)
    tbl, refs = build_table(wb, data)
    build_graph(wb, tbl, refs)
    build_runs(wb, rows)
    wb.save(OUT)
    print(OUT, wb.sheetnames, {k: refs[k] for k in refs})


if __name__ == '__main__':
    main()
