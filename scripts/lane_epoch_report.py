#!/usr/bin/env python3
"""Analyse a --lane-epoch-us CSV written by the musq-testapp responder.

Each lane (core) sampled its own counters once per epoch on a common TSC grid,
so records with the same epoch number were taken at (almost) the same instant on
every lane. This lines them up and reports:

  RX         packets each lane received per epoch (mean, min, max, std dev), each
             lane's share, and the balance across lanes per epoch: spread
             (max - min), relative spread and Jain's fairness index; also how far
             the lanes' cumulative RX counts drift apart
  CPU        per lane: busy % (loop passes that handled packets, including mock
             work, against empty polls) over the run and per epoch, and busy
             cycles per received packet
  TX         per lane: packets posted, slots, short slots (fewer packets than a
             slot holds), packets per slot, TX drops, free WQEBBs (mean, min),
             and, when the log has them, doorbells per second and slots (WQEs)
             per doorbell
  skew       slots posted by the fastest lane beyond the slowest, per epoch. One
             slot is one round of the shared ring, so this is how far a lane is
             ahead. Reported as a distribution (p50/p90/p99/max), a least-squares
             trend, and the excursions above a threshold with their cause: the
             difference in packets received and in TX drops between the leading
             and the lagging lane at the peak
  rings      share of epochs in which all lanes, only some, or none are full: all
             full means the NIC is the limit, only some means skew is the limit
  frontier   spread of wqe_pi across lanes (in rounds), which checks that the
             NIC completes every lane up to the same position
  CQ         per lane: completion requests made and not consumed (in use) against
             the lane's share of the CQ, and how many of them the NIC has already
             completed (done): a large "done" means the lane is behind the NIC,
             a small one with the CQ in use means the NIC is behind the lane
  timeline   evenly spaced rows of the above

The TX, skew, rings and frontier sections need the WQ columns (prom-dpdk mlx5
PMD, DOCA build); a log recorded without them reports RX only. The CQ section
needs the CQ columns (a PMD with rte_pmd_mlx5_txq_cq_status).

    scripts/lane_epoch_report.py lane_epoch.csv [--rows 20] [--active-only]
"""
import argparse
import csv
import math
import sys
from collections import defaultdict

DEFAULT_SLOT_WQEBBS = 9  # WQEBBs per 32-packet mu_sq slot; the CSV header overrides it


def s16(x):
    x &= 0xFFFF
    return x - 65536 if x >= 32768 else x


def load(path):
    meta = {}
    lanes = defaultdict(dict)
    with open(path) as f:
        first = f.readline()
        if first.startswith("#"):
            for tok in first[1:].split():
                k, _, v = tok.partition("=")
                meta[k] = int(v)
        else:
            f.seek(0)
        for row in csv.DictReader(f):
            r = {k: (float(v) if k in ("t_us", "busy_pct") else int(v)) for k, v in row.items()}
            lanes[r["lane"]][r["epoch"]] = r
    return meta, lanes


def pct(sorted_vals, p):
    return sorted_vals[min(len(sorted_vals) - 1, int(p / 100.0 * len(sorted_vals)))]


def lsq_slope(xs, ys):
    """Least-squares slope of ys over xs."""
    n = len(xs)
    if n < 2:
        return 0.0
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--rows", type=int, default=16, help="timeline rows to print")
    ap.add_argument("--active-only", action="store_true",
                    help="restrict to the span where traffic was flowing (RX counters advancing)")
    ap.add_argument("--threshold", type=int, default=0,
                    help="skew (slots) above which an excursion is listed (default: max(100, 3 x p99))")
    ap.add_argument("--excursions", type=int, default=8, help="how many excursions to list (the largest)")
    a = ap.parse_args()

    meta, lanes = load(a.csv)
    ids = sorted(lanes)
    n = len(ids)
    if n == 0:
        sys.exit("no records")
    epoch_us = meta.get("epoch_us", 0)
    slot_wqebbs = meta.get("slot_wqebbs", DEFAULT_SLOT_WQEBBS)
    slot_pkts = meta.get("slot_pkts", 32)
    have_wq = meta.get("wq", 1) == 1 and any(r["slots"] or r["wqe_ci"] for i in ids for r in lanes[i].values())
    common = sorted(set.intersection(*(set(lanes[i]) for i in ids)))
    if len(common) < 2:
        sys.exit("fewer than two epochs common to all lanes")

    def total_rx(e):
        return sum(lanes[i][e]["rx_pkts"] for i in ids)

    if a.active_only:
        act = [e for k, e in enumerate(common[1:], 1) if total_rx(e) != total_rx(common[k - 1])]
        if len(act) < 2:
            sys.exit("no epochs with traffic")
        common = [e for e in common if act[0] - 1 <= e <= act[-1]]
    e0, e1 = common[0], common[-1]
    t0_us = lanes[ids[0]][e0]["t_us"]
    tsec = [(lanes[ids[0]][e]["t_us"] - t0_us) / 1e6 for e in common]
    dur_s = tsec[-1]

    print(f"{a.csv}: {n} lanes, epoch {epoch_us} us, {len(common)} common epochs, {dur_s:.3f} s"
          f"{' (active span)' if a.active_only else ''}")
    missing = (e1 - e0 + 1) - len(common)
    if missing:
        print(f"  note: {missing} epochs in the span are missing on at least one lane (a loop pass took longer than an epoch)")
    jit = max(max(lanes[i][e]["t_us"] for i in ids) - min(lanes[i][e]["t_us"] for i in ids) for e in common)
    print(f"  largest sample-time spread between lanes within one epoch: {jit:.1f} us")

    # ---- RX packets per lane per epoch -------------------------------------------------
    # Only consecutive epochs give a per-epoch count; a gap would merge several epochs.
    pairs = [(common[k - 1], common[k]) for k in range(1, len(common)) if common[k] == common[k - 1] + 1]
    rx = {i: [lanes[i][b]["rx_pkts"] - lanes[i][p]["rx_pkts"] for p, b in pairs] for i in ids}
    tot_rx = {i: lanes[i][e1]["rx_pkts"] - lanes[i][e0]["rx_pkts"] for i in ids}
    all_rx = sum(tot_rx.values())
    print(f"\nRX packets per lane per {epoch_us} us epoch ({len(pairs)} epochs):")
    print(f"{'lane':>4} {'received':>14} {'share':>7} {'pps':>13} {'mean/epoch':>11} {'min':>8} {'max':>8} {'std dev':>8}")
    for i in ids:
        v = rx[i]
        mean = sum(v) / len(v) if v else 0
        sd = math.sqrt(sum((x - mean) ** 2 for x in v) / len(v)) if v else 0
        print(f"{i:>4} {tot_rx[i]:>14,} {100.0 * tot_rx[i] / all_rx if all_rx else 0:>6.2f}% "
              f"{tot_rx[i] / dur_s if dur_s else 0:>13,.0f} {mean:>11.1f} {min(v) if v else 0:>8} "
              f"{max(v) if v else 0:>8} {sd:>8.1f}")
    spreads, rels, jains = [], [], []
    for k in range(len(pairs)):
        v = [rx[i][k] for i in ids]
        s = sum(v)
        if s == 0:
            continue
        spreads.append(max(v) - min(v))
        rels.append((max(v) - min(v)) / (s / n))
        jains.append(s * s / (n * sum(x * x for x in v)))
    if spreads:
        ss, rs, js = sorted(spreads), sorted(rels), sorted(jains)
        print("  balance across lanes within an epoch (epochs with traffic):")
        print(f"    spread max-min: mean {sum(ss) / len(ss):.1f} pkts, p50 {pct(ss, 50)}, p99 {pct(ss, 99)}, max {ss[-1]}")
        print(f"    relative to the per-lane mean: mean {100 * sum(rs) / len(rs):.2f}%, p99 {100 * pct(rs, 99):.2f}%, max {100 * rs[-1]:.2f}%")
        print(f"    Jain fairness index (1 = equal): mean {sum(js) / len(js):.6f}, min {js[0]:.6f}")

    def since(i, e, key):
        return lanes[i][e][key] - lanes[i][e0][key]

    def rx_ahead(e):
        v = [since(i, e, "rx_pkts") for i in ids]
        m = min(v)
        return [x - m for x in v]

    cum = [max(rx_ahead(e)) for e in common]
    print(f"  cumulative RX, packets ahead of the lane with fewest: max {max(cum)}, at end {rx_ahead(e1)}")

    # ---- CPU usage per lane (needs the cycles_busy / cycles_idle columns) --------------
    have_cpu = all("cycles_busy" in lanes[i][e0] for i in ids) and \
        any(since(i, e1, "cycles_busy") + since(i, e1, "cycles_idle") for i in ids)
    busy = {}
    if have_cpu:
        def busy_pct(i, p, b):
            db = lanes[i][b]["cycles_busy"] - lanes[i][p]["cycles_busy"]
            di = lanes[i][b]["cycles_idle"] - lanes[i][p]["cycles_idle"]
            return 100.0 * db / (db + di) if db + di else 0.0
        busy = {i: [busy_pct(i, p, b) for p, b in pairs] for i in ids}
        print(f"\nCPU per lane (busy = loop passes that handled packets, incl. mock work; idle = empty polls):")
        print(f"{'lane':>4} {'busy %':>7} {'per epoch: min':>15} {'p50':>6} {'p99':>6} {'max':>6} {'busy cycles/RX pkt':>19} {'idle %':>7}")
        for i in ids:
            db, di = since(i, e1, "cycles_busy"), since(i, e1, "cycles_idle")
            v = sorted(busy[i]) or [0.0]
            tot = 100.0 * db / (db + di) if db + di else 0.0
            print(f"{i:>4} {tot:>6.1f}% {v[0]:>14.1f}% {pct(v, 50):>5.1f}% {pct(v, 99):>5.1f}% {v[-1]:>5.1f}% "
                  f"{db / tot_rx[i] if tot_rx[i] else 0:>19.1f} {100.0 - tot:>6.1f}%")

    # ---- TX slots, skew, rings, frontier (need the PMD's WQ columns) ------------------
    if have_wq:
        stride = slot_wqebbs * n  # one lane per worker
        ring = meta.get("wqe_s", 0) or max(r["wqe_free"] for i in ids for r in lanes[i].values())
        rounds = ring // stride if stride else 0
        print(f"\nmu_sq slot: {slot_pkts} packet(s), {slot_wqebbs} WQEBB(s); one round = {stride} WQEBBs;"
              f" ring {ring} WQEBBs = {rounds} rounds per lane")
        have_db = meta.get("db", 0) == 1 and all("doorbells" in lanes[i][e0] for i in ids)
        print("TX per lane over the span:" +
              (f" (doorbell every {meta['db_wqes']} WQEs requested)" if have_db and meta.get("db_wqes") else ""))
        print(f"{'lane':>4} {'packets':>13} {'pps':>12} {'slots':>11} {'short slots':>11} {'short/s':>9} {'pkts/slot':>9} "
              f"{'tx drops':>10} {'free mean':>9} {'free min':>8}"
              + (f" {'doorbells/s':>12} {'slots/doorbell':>14}" if have_db else ""))
        for i in ids:
            slots = since(i, e1, "slots")
            pk = since(i, e1, "slot_pkts")
            short = slots - since(i, e1, "slots_full")
            frees = [lanes[i][e]["wqe_free"] for e in common]
            line = (f"{i:>4} {pk:>13,} {pk / dur_s if dur_s else 0:>12,.0f} {slots:>11,} {short:>11,} "
                    f"{short / dur_s if dur_s else 0:>9,.0f} {pk / slots if slots else 0:>9.3f} "
                    f"{since(i, e1, 'tx_drops'):>10,} {sum(frees) / len(frees):>9.0f} {min(frees):>8}")
            if have_db:
                db = since(i, e1, "doorbells")
                line += f" {db / dur_s if dur_s else 0:>12,.0f} {slots / db if db else 0:>14.1f}"
            print(line)

        def ahead(e):
            v = [lanes[i][e]["slots"] for i in ids]
            m = min(v)
            return [x - m for x in v]

        skews, leaders, laggers = [], [], []
        for e in common:
            v = [lanes[i][e]["slots"] for i in ids]
            hi, lo = max(v), min(v)
            skews.append(hi - lo)
            leaders.append(v.index(hi))
            laggers.append(v.index(lo))
        sk = sorted(skews)
        p99 = pct(sk, 99)
        print("\nlane skew (slots the fastest lane is ahead of the slowest; one slot = one round):")
        print(f"  over all epochs: p50 {pct(sk, 50)}, p90 {pct(sk, 90)}, p99 {p99}, max {sk[-1]}"
              f"  (a lane {rounds} rounds ahead has a full ring)")
        line = f"  trend (least-squares fit over the span): {lsq_slope(tsec, skews):+.2f} slots/s"
        if dur_s >= 2.0:
            head = [s for t, s in zip(tsec, skews) if t <= 1.0]
            tail = [s for t, s in zip(tsec, skews) if t >= dur_s - 1.0]
            line += f"; mean in the first second {sum(head) / len(head):.1f}, in the last second {sum(tail) / len(tail):.1f}"
        print(line)
        lead = defaultdict(int)
        for k, s in enumerate(skews):
            if s > 0:
                lead[ids[leaders[k]]] += 1
        print("  leading lane, share of epochs: " +
              ", ".join(f"lane {i}: {100.0 * lead[i] / len(common):.0f}%" for i in ids))

        thr = a.threshold or max(100, 3 * p99)
        exc = []
        k = 0
        while k < len(common):
            if skews[k] > thr:
                j = k
                while j < len(common) and skews[j] > thr:
                    j += 1
                pk = max(range(k, j), key=lambda x: skews[x])
                exc.append((k, j, pk))
                k = j
            else:
                k += 1
        print(f"  excursions above {thr} slots: {len(exc)}"
              + (f" (the {min(len(exc), a.excursions)} largest, in time order)" if exc else ""))
        if exc:
            note = "skew = rx diff - drop diff, within one TX buffer" if slot_pkts == 1 else \
                   "with multi-packet slots the skew also depends on how full each lane's slots are"
            print(f"    at the peak, leading minus lagging lane since the start of the span ({note}):")
            print(f"    {'t ms':>10} {'dur ms':>7} {'peak':>6} {'lead':>4} {'lag':>4} {'rx diff':>8} {'drop diff':>9} "
                  f"{'short-slot diff':>15} {'free lead/lag':>14}")
            top = sorted(sorted(exc, key=lambda x: -skews[x[2]])[: a.excursions])
            for k, j, pk in top:
                e = common[pk]
                L, l = ids[leaders[pk]], ids[laggers[pk]]
                short = lambda i: since(i, e, "slots") - since(i, e, "slots_full")
                print(f"    {tsec[k] * 1e3:>10.1f} {(tsec[j - 1] - tsec[k]) * 1e3 + epoch_us / 1e3:>7.1f} {skews[pk]:>6} {L:>4} {l:>4} "
                      f"{since(L, e, 'rx_pkts') - since(l, e, 'rx_pkts'):>+8} "
                      f"{since(L, e, 'tx_drops') - since(l, e, 'tx_drops'):>+9} {short(L) - short(l):>+15} "
                      f"{lanes[L][e]['wqe_free']:>7}/{lanes[l][e]['wqe_free']:<6}")

        full_thr = max(stride * 4, ring // 16)
        allf = somef = 0
        for e in common:
            c = sum(1 for i in ids if lanes[i][e]["wqe_free"] < full_thr)
            if c == n:
                allf += 1
            elif c > 0:
                somef += 1
        tot = len(common)
        print(f"\nrings (full = fewer than {full_thr} WQEBBs free): all lanes full in {100.0 * allf / tot:.1f}% of epochs,"
              f" only some in {100.0 * somef / tot:.1f}%, none in {100.0 * (tot - allf - somef) / tot:.1f}%")
        print("  all full: the NIC completes slower than the lanes post. Only some full: those lanes are ahead"
              " and wait for the slowest lane.")

        fr = []
        for e in common:
            base = lanes[ids[0]][e]["wqe_pi"]
            rel = [(s16(lanes[i][e]["wqe_pi"] - base) - slot_wqebbs * k) / stride for k, i in enumerate(ids)]
            fr.append(max(rel) - min(rel))
        print("\ncompletion frontier (spread of wqe_pi across lanes, in rounds):")
        print(f"  mean {sum(fr) / len(fr):.2f}, max {max(fr):.2f}"
              f"  (small means the NIC completes all lanes up to the same position;"
              f" each lane sees it one completion, i.e. up to one TX burst, at a time)")
    else:
        print("\n(no WQ columns in this log: TX, skew, rings and frontier sections skipped)")

    # ---- CQ per lane (needs the PMD's CQ columns) -------------------------------------
    have_cq = meta.get("cq", 0) == 1 and all("cq_free" in lanes[i][e0] for i in ids)
    if have_cq:
        share = meta.get("cqe_s", 0) // max(1, meta.get("cq_step", 1))
        print(f"\nCQ per lane: {share} entries (CQ of {meta.get('cqe_s', 0)} entries,"
              f" 1 in {meta.get('cq_step', 1)} is a lane's own)")
        print("  in use = completion requests made and not consumed by the lane;"
              " done = of those, already completed by the NIC")
        print(f"{'lane':>4} {'in use mean':>11} {'p99':>6} {'max':>6} {'free min':>8} {'no free entry':>13} "
              f"{'done mean':>9} {'p50':>6} {'p99':>6} {'max':>6} {'done/in use':>11}")
        for i in ids:
            used = sorted(share - lanes[i][e]["cq_free"] for e in common)
            done = sorted(lanes[i][e]["cq_done"] for e in common)
            nofree = sum(1 for e in common if lanes[i][e]["cq_free"] == 0)
            su, sd = sum(used), sum(done)
            print(f"{i:>4} {su / len(used):>11.1f} {pct(used, 99):>6} {used[-1]:>6} {share - used[-1]:>8} "
                  f"{100.0 * nofree / len(common):>12.1f}% {sd / len(done):>9.1f} {pct(done, 50):>6} "
                  f"{pct(done, 99):>6} {done[-1]:>6} {100.0 * sd / su if su else 0.0:>10.1f}%")

    # ---- timeline ---------------------------------------------------------------------
    hdr = f"{'t ms':>9}  " + " ".join(f"{'rx' + str(i):>7}" for i in ids)
    if have_cpu:
        hdr += "  | " + " ".join(f"{'cpu' + str(i):>5}" for i in ids)
    if have_wq:
        hdr += "  | " + " ".join(f"{'ahead' + str(i):>7}" for i in ids) + "  | " + " ".join(f"{'free' + str(i):>6}" for i in ids)
    print(f"\ntimeline ({a.rows} of {len(common)} epochs): RX packets in the epoch"
          + (" | CPU busy % in the epoch" if have_cpu else "")
          + (" | slots ahead of the slowest lane | free WQEBBs" if have_wq else ""))
    print(hdr)
    idx = {b: k for k, (p, b) in enumerate(pairs)}
    step = max(1, (len(common) - 1) // max(1, a.rows - 1))
    for e in common[::step][: a.rows - 1] + [common[-1]]:
        t = (lanes[ids[0]][e]["t_us"] - t0_us) / 1000.0
        line = f"{t:>9.1f}  " + " ".join(f"{(rx[i][idx[e]] if e in idx else 0):>7}" for i in ids)
        if have_cpu:
            line += "  | " + " ".join(f"{(busy[i][idx[e]] if e in idx else 0.0):>5.1f}" for i in ids)
        if have_wq:
            line += "  | " + " ".join(f"{x:>7}" for x in ahead(e)) + "  | " + \
                    " ".join(f"{lanes[i][e]['wqe_free']:>6}" for i in ids)
        print(line)


if __name__ == "__main__":
    main()
