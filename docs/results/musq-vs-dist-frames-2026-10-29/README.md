# mu_sq against the best distributor at 2 worker cores: frame-size sweep (2026-10-29)

The user's request: "Compare throughput of musq and distributor by sweeping
frame size (128, 256, 512, 1024) with 2 worker cores." Layouts: mu_sq with 2
lanes (lcores 2 and 4; the finding-16 command, 1 packet per WQE, txd 2048,
`mu_sq_comp_n=128`, `--db-wqes 128`) and the best distributor with 1 master +
2 workers (master lcore 2, workers 4 and 6; bursts 64, chunk 32, `--dist-self
32`, finding 20). Requester on danger50 with 8 lcores, ESP frames encapsulated
by its NIC at the requested sizes 128 / 256 / 512 / 1024 B (126 / 254 / 510 /
1022 B on the wire); without work, and with 100 and 200 ns of mock work per
packet. Hosts as restored that morning (hugepages, pause off, performance
governor on CPUs 0 and 2 to 18, numa_balancing 0 on danger51).

Two methods, as in findings 16 to 20: **saturation runs** (80 Mpps offered,
which at 256 B and up means what the requester's line allows: 73 / 54 / 27
Mpps; the forwarded rate over the steady window) and the **strict rule** at
the no-work walls (TX refused and RX loss each <= 0.01 % over the steady
window, three passing runs, 0.5 Mpps steps). The workbook
[../musq-vs-distributor-frames-2026-10-29.xlsx](../musq-vs-distributor-frames-2026-10-29.xlsx)
holds the charts. Data: [sat/](sat/), [strict/](strict/), the variant with two
RX queues on the master in [sat-rxq2/](sat-rxq2/) and [strict-rxq2/](strict-rxq2/).
Harness: [harness/](harness/) (`stageE.sh`; `strict_points.py` takes a frame
now). Runs 13:13 to 13:37 and 13:38 to 13:49.

## Without work: the walls against the frame size

| frame | wire B | mu_sq 2 lanes: loss-free / saturated Mpps | distributor 1 + 2: loss-free / saturated | distributor, master 2 RX queues: loss-free / saturated | distributor vs mu_sq (loss-free) | 2 RX queues vs mu_sq |
|---|---|---|---|---|---|---|
| 128 B | 126 | 40.5 / 40.9 (refused 37 %) | 34.0 / 34.2 | 29.0 / 35.8 | -16 % | -28 % |
| 256 B | 254 | 38.0 / 36.6 (refused 40 %) | 30.5 / 31.4 | 29.5 / 35.5 | -20 % | -22 % |
| 512 B | 510 | 29.5 / 26.5 (refused 44 %) | 25.0 / 25.1 | 30.0 / 31.3 | -15 % | +2 % |
| 1024 B | 1022 | 19.5 / 12.6 (refused 54 %) | 16.75 / 16.8 | 22.0 / 22.2 | -14 % | +13 % |

- **mu_sq's loss-free ceiling is above the distributor's at every frame
  size**, by 19 % at 128 B, 25 % at 256 B, 18 % at 512 B and 16 % at 1024 B:
  the shared send queue against one core's RX + TX. mu_sq fails on TX refusals
  at the next step, the distributor on RX loss at its master.
- **Under overload mu_sq forwards much less than it can without loss** once
  the frames are large: 26.5 Mpps saturated against 29.5 loss-free at 512 B,
  12.6 against 19.5 at 1024 B, with 44 to 54 % of the packets refused at TX.
  A refused packet has cost its lane the RX and the send attempt, and the
  shared queue's slots stay occupied by them; at 128 and 256 B the two numbers
  are within 4 %. The distributor's two numbers agree within 1 to 3 % at every
  size (its master holds RX when it cannot send, nothing is refused). Any
  comparison of mu_sq at large frames must therefore use the loss-free
  ceiling, not an overload run.
- **The distributor's single RX queue is the limit at 512 and 1024 B** (the
  master at 98 % busy at 512 B but only 80 % at 1024 B with 16.8 Mpps of 27
  offered): with `--rx-queues 2` on the master (RSS over two queues, order
  kept per flow, not across flows) it forwards 31.3 Mpps at 512 B and 22.2 at
  1024 B saturated, 30.0 and 22.0 loss-free, i.e. 2 and 13 % above mu_sq's
  ceilings there. But at 128 and 256 B the same setting lowers the loss-free
  ceiling to 29.0 and 29.5 (one queue: 34.0 and 30.5) while the saturated
  rate rises to 35.8 and 35.5: the eight flows split unevenly over the two
  queues and the fuller one drops first, which the strict rule counts and an
  overload run hides. The right master setting therefore depends on the
  frame size: one RX queue up to 256 B, two from 512 B on.

## With work: 100 and 200 ns per packet (saturated runs)

| work/pkt | frame | mu_sq Mpps (refused) | distributor Mpps (master busy, cyc/pkt, own share) | distributor vs mu_sq |
|---|---|---|---|---|
| 100 ns | 128 B | 11.8 (0.4 %) | 17.9 (99 %, 133, 3.83 Mpps) | +52 % |
| 100 ns | 256 B | 11.8 (0.4 %) | 17.9 (99 %, 133, 3.80 Mpps) | +52 % |
| 100 ns | 512 B | 11.8 (0.4 %) | 17.8 (99 %, 133, 3.75 Mpps) | +52 % |
| 100 ns | 1024 B | 11.8 (0.3 %) | 16.8 (91 %, 130, 2.70 Mpps) | +42 % |
| 200 ns | 128 B | 7.8 (0.1 %) | 11.8 (99 %, 201, 3.07 Mpps) | +51 % |
| 200 ns | 256 B | 7.8 (0.2 %) | 11.8 (99 %, 201, 3.06 Mpps) | +51 % |
| 200 ns | 512 B | 7.8 (0.3 %) | 11.8 (99 %, 202, 3.04 Mpps) | +50 % |
| 200 ns | 1024 B | 7.8 (0.2 %) | 11.8 (99 %, 201, 3.02 Mpps) | +50 % |

- With work the frame size does not matter to either layout (the cores are
  the limit, 11.8 / 7.8 Mpps for mu_sq, 17.9 / 11.8 for the distributor,
  mu_sq refusing under 0.5 %): the distributor is ahead by 50 to 52 %, its
  extra core, as in the worker-count view of finding 20. The one exception is
  1024 B at 100 ns, where the distributor's single RX queue caps it at 16.8
  (+42 %), the same cap as without work.

## Summary

- At 2 worker cores and no work, mu_sq's loss-free ceiling leads the
  distributor's by 16 to 25 % across 128 to 1024 B (40.5 / 38.0 / 29.5 / 19.5
  against 34.0 / 30.5 / 25.0 / 16.75 Mpps); with two RX queues on the master
  the distributor passes mu_sq at 512 and 1024 B (30.0 and 22.0, +2 and
  +13 %) but loses at 128 and 256 B (29.0 and 29.5), so the queue count is
  a per-frame-size choice.
- Under overload mu_sq collapses at large frames (12.6 Mpps at 1024 B against
  19.5 loss-free) because of its TX refusals; the distributor does not.
- With 100 or 200 ns of work per packet the distributor is 50 to 52 % ahead
  at every frame size (42 % at 1024 B and 100 ns, its single-RX-queue cap).
