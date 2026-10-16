#!/usr/bin/env python3
"""Turn a --lane-epoch-us CSV into a table sorted by time.

The responder writes the log lane by lane (all of lane 0, then lane 1, ...).
This writes a CSV that is easier to read or plot:

  wide (default)  one row per epoch, in time order, with the lanes side by side:
                    epoch, t_us (earliest sample of the epoch), t_spread_us,
                    per lane: rx, tx, drops (packets in the epoch), busy_pct,
                              free (WQEBBs), used (WQEBBs), ahead (slots ahead of
                              the slowest lane), cq_free (free entries of the
                              lane's share of the CQ), cq_done (completions the
                              NIC has written that the lane has not consumed),
                              db (doorbells the PMD rang in the epoch),
                    then rx_total, tx_total, drops_total, skew (max - min slots)
  --long          one row per lane and epoch, sorted by t_us then lane: the
                  original columns plus rx_epoch, tx_epoch, drops_epoch, busy_pct

Per-epoch values are the difference to the same lane's previous record; they
are left empty when that record is missing. Columns the log does not have (WQ
position without the prom-dpdk PMD, CQ state and CPU cycles in older logs) are
left out.
By default only the span with traffic is written (plus 10 epochs before it);
--all keeps every epoch.

    scripts/lane_epoch_table.py lane_epoch.csv                 # -> lane_epoch_table.csv
    scripts/lane_epoch_table.py lane_epoch.csv --long -o out.csv
"""
import argparse
import csv
import os
import sys
from collections import defaultdict


def load(path):
    meta = {}
    lanes = defaultdict(dict)
    fields = []
    with open(path) as f:
        first = f.readline()
        if first.startswith("#"):
            for tok in first[1:].split():
                k, _, v = tok.partition("=")
                meta[k] = int(v)
        else:
            f.seek(0)
        rd = csv.DictReader(f)
        fields = rd.fieldnames or []
        for row in rd:
            r = {k: (float(v) if k in ("t_us", "busy_pct") else int(v)) for k, v in row.items()}
            lanes[r["lane"]][r["epoch"]] = r
    return meta, lanes, fields


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("-o", "--out", help="output file (default: <input>_table.csv or <input>_long.csv)")
    ap.add_argument("--long", action="store_true", help="one row per lane and epoch instead of one row per epoch")
    ap.add_argument("--all", action="store_true", help="keep the idle epochs before and after the traffic")
    a = ap.parse_args()

    meta, lanes, fields = load(a.csv)
    ids = sorted(lanes)
    if not ids:
        sys.exit("no records")
    have_wq = meta.get("wq", 1) == 1 and any(r["slots"] or r["wqe_ci"] for i in ids for r in lanes[i].values())
    have_cpu = "cycles_busy" in fields
    have_cq = meta.get("cq", 0) == 1 and "cq_free" in fields
    have_db = meta.get("db", 0) == 1 and "doorbells" in fields
    wqe_s = meta.get("wqe_s", 0) or max(r["wqe_free"] for i in ids for r in lanes[i].values())

    epochs = sorted(set().union(*(set(lanes[i]) for i in ids)))
    if not a.all:
        tot = {e: sum(lanes[i][e]["rx_pkts"] for i in ids if e in lanes[i]) for e in epochs}
        act = [e for k, e in enumerate(epochs[1:], 1) if tot[e] != tot[epochs[k - 1]]]
        if act:
            lo, hi = act[0] - 10, act[-1]
            epochs = [e for e in epochs if lo <= e <= hi]

    def delta(i, e, key):
        cur, prev = lanes[i].get(e), lanes[i].get(e - 1)
        if cur is None or prev is None:
            return ""
        return cur[key] - prev[key]

    def busy_pct(i, e):
        cur, prev = lanes[i].get(e), lanes[i].get(e - 1)
        if cur is None or prev is None:
            return ""
        db = cur["cycles_busy"] - prev["cycles_busy"]
        di = cur["cycles_idle"] - prev["cycles_idle"]
        return f"{100.0 * db / (db + di):.1f}" if db + di else "0.0"

    def used(r):
        return (r["wqe_ci"] - r["wqe_pi"]) & 0xFFFF

    t0 = min(lanes[i][e]["t_us"] for e in epochs[:1] for i in ids if e in lanes[i]) if epochs else 0.0
    base = os.path.splitext(a.csv)[0]
    out = a.out or f"{base}_{'long' if a.long else 'table'}.csv"
    n_rows = 0
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        if a.long:
            # busy_pct comes from the responder in newer logs; compute it only for older ones.
            add_busy = have_cpu and "busy_pct" not in fields
            extra = ["rx_epoch", "tx_epoch", "drops_epoch"] + (["busy_pct"] if add_busy else [])
            w.writerow(fields + extra)
            rows = [(lanes[i][e]["t_us"], i, e) for e in epochs for i in ids if e in lanes[i]]
            rows.sort()
            for t, i, e in rows:
                r = lanes[i][e]
                vals = [f"{r[k]:.1f}" if k in ("t_us", "busy_pct") else r[k] for k in fields]
                vals += [delta(i, e, "rx_pkts"), delta(i, e, "tx_pkts"), delta(i, e, "tx_drops")]
                if add_busy:
                    vals.append(busy_pct(i, e))
                w.writerow(vals)
                n_rows += 1
        else:
            hdr = ["epoch", "t_us", "t_spread_us"]
            for i in ids:
                hdr += [f"rx{i}", f"tx{i}", f"drops{i}"]
                if have_cpu:
                    hdr.append(f"busy_pct{i}")
                if have_wq:
                    hdr += [f"free{i}", f"used{i}", f"ahead{i}"]
                if have_cq:
                    hdr += [f"cq_free{i}", f"cq_done{i}"]
                if have_db:
                    hdr.append(f"db{i}")
            hdr += ["rx_total", "tx_total", "drops_total"] + (["skew"] if have_wq else [])
            w.writerow(hdr)
            for e in epochs:
                present = [i for i in ids if e in lanes[i]]
                if not present:
                    continue
                ts = [lanes[i][e]["t_us"] for i in present]
                row = [e - epochs[0], f"{min(ts) - t0:.1f}", f"{max(ts) - min(ts):.1f}"]
                slots = {i: lanes[i][e]["slots"] for i in present}
                lo = min(slots.values())
                tots = [0, 0, 0]
                for i in ids:
                    d = [delta(i, e, k) for k in ("rx_pkts", "tx_pkts", "tx_drops")]
                    row += d
                    for k in range(3):
                        if d[k] != "":
                            tots[k] += d[k]
                    if have_cpu:
                        row.append(busy_pct(i, e))
                    if have_wq:
                        r = lanes[i].get(e)
                        row += [r["wqe_free"], used(r), slots[i] - lo] if r else ["", "", ""]
                    if have_cq:
                        r = lanes[i].get(e)
                        row += [r["cq_free"], r["cq_done"]] if r else ["", ""]
                    if have_db:
                        row.append(delta(i, e, "doorbells"))
                row += tots
                if have_wq:
                    row.append(max(slots.values()) - lo if len(slots) == len(ids) else "")
                w.writerow(row)
                n_rows += 1
    print(f"wrote {out}: {n_rows} rows, {len(ids)} lanes, {meta.get('epoch_us', '?')} us epochs"
          f"{'' if a.all else ' (span with traffic)'}; WQ columns {'yes' if have_wq else 'no'},"
          f" CQ columns {'yes' if have_cq else 'no'}, doorbells {'yes' if have_db else 'no'},"
          f" CPU columns {'yes' if have_cpu else 'no'};"
          f" ring {wqe_s} WQEBBs")


if __name__ == "__main__":
    main()
