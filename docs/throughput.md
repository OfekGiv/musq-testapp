# musq-testapp throughput

Measured 2026-09-29. This page records how fast the requester (traffic
generator) and the plain RSS responder can go on danger50/danger51, what limits
each of them, and how to reproduce the numbers.

## Setup

| | danger50 (requester) | danger51 (responder) |
|---|---|---|
| CPU | Xeon Silver 4314, 2.4 GHz, no turbo, NUMA 0 = even CPUs | same |
| NIC | ConnectX-9 `0000:17:00.1` (`enp23s0f1np1`), 400 Gb/s link | same |
| PCIe | Gen4 x16 (`LnkSta: 16GT/s (downgraded)`) | Gen4 x16 (capable of 64 GT/s) |
| DPDK | `/opt/mellanox/dpdk` (pmds-26.1), `build-nodoca` | prom-dpdk `install-o2-nometa`, `build-nodoca` |

**Link-level flow control is off on both ports** since 2026-09-29:

```sh
sudo ethtool -A enp23s0f1np1 rx off tx off   # check with: sudo ethtool -a enp23s0f1np1
```

This is not persistent. After a reboot or driver reload global pause is on again,
and any receiver that falls behind then pauses the link and throttles the other
side's TX (see [Pause frames](#pause-frames)). Check it before every comparison.

Unless noted, requester runs use `--doca-mode none --musq-group 0 --traffic udp`
and the responder uses `--doca-mode none --musq-group 0 --tx-mode immediate
--work fixed:0` (plain RSS, no mu_sq). Frame sizes are on the wire including FCS.

## Requester TX

### How to measure

Stop the responder and send to a MAC danger51 does not own, so danger51's NIC
discards the frames in hardware and nothing is echoed back:

```sh
# danger50
C="--role requester --doca-mode none --musq-group 0 --traffic udp \
   --dst-mac 02:00:00:00:00:01 --warmup 1 --duration 10 --stats-interval 2"
scripts/run.sh --lcores 2 $C --frame-size 128 --rps 150000000
scripts/run.sh --lcores 2 $C --frame-size 128 --rps 150000000 --txd 4096 --tracker-size 65536
scripts/run.sh --lcores 2,4 $C --frame-size 512 --rps 40000000
```

Read "TX pps" and "TX Gbps (wire)" in the requester summary. A non-zero per-core
"TX drops" means `rte_eth_tx_burst` refused packets because the TX ring was full,
so the NIC side, not the lcore, is the limit. Drops (TX-RX), Req lost and the
empty RTT table are expected in this mode.

### One lcore

| frame | `--tx-fill copy` | `--tx-fill prefill` (default) | prefill, `--txd 4096 --tracker-size 65536` |
|---|---|---|---|
| 128 B | 41 Mpps | 74 Mpps | 80 Mpps |
| 512 B | 21 Mpps | 45 Mpps | 50 Mpps |

`copy` is the korba way of building a request: `rte_pktmbuf_alloc_bulk` and a
`rte_memcpy` of the whole frame template per packet. At 512 B the copy alone took
about half of the lcore. `prefill` gives every lcore a dedicated TX mempool whose
mbufs carry the template from start-up; per packet only the request id, the
source port or ESP sequence number, and any field that varies (destination port
range, non-fixed service time) are written.

How one lcore at 128 B got from 46.6 to 74 Mpps (defaults, 1 M-slot tracker):

| change | Mpps | cycles/pkt |
|---|---|---|
| prefill with `rte_pktmbuf_alloc_bulk` | 46.6 | 52 |
| raw `rte_mempool_get_bulk`, lengths set once at prefill (no per-mbuf reset, which touched the mbuf's second cache line) | 59 | 40 |
| prefetch tracker slots ahead | 61 | 38 |
| per-batch fill function with all cursors in registers; fixed fields in the template | 73 | 32 |
| prefetch mbuf headers and packet data | 72 | 32 |
| fill function kept out of line (fewer register spills) | 74 | 31 |

Where the remaining ~30 cycles/packet go at 128 B: IPC is about 3 and last-level
cache misses are negligible, so the lcore is instruction-bound. About 20 cycles
are the mempool get and the mlx5 TX path, about 10 are the request fields and the
tracker slot. With no per-packet writes at all, one TX queue reaches 75 Mpps with
1024 descriptors, 96 Mpps with 4096 and 104 Mpps with 8192.

The default 1 M-slot tracker (32 MB per lcore) costs about 5 Mpps against a
64 k-slot one, because its slots stream from DRAM. A smaller tracker is fine as
long as `--tracker-size` exceeds rps per lcore × the longest RTT to be measured.

### Several lcores

| frame | 1 lcore | 2 lcores | 4 lcores | 8 lcores |
|---|---|---|---|---|
| 512 B | 45 Mpps | 54.7 Mpps / 224 Gbps | 53.8 Mpps | 55.0 Mpps |
| 128 B | 47 Mpps | 88 Mpps | 160 Mpps (capped by `--rps 40000000` per lcore) | |

The 128 B multi-lcore rows were measured before the single-lcore optimizations
above, so they are lower bounds. At 512 B two lcores already reach the **PCIe
Gen4 x16 limit**: 54.2 M × 508 B ≈ 27.5 GB/s of payload read by the NIC, about
87 % of the raw link; the rest is TLP headers, descriptor reads and completions.
No lcore count or ring depth moves this.

### Pause frames

With global pause on and an echoing responder, the requester's TX ring filled and
TX stalled far below the PCIe limit. Both hosts showed pause frames in both
directions in `ethtool -S` (`rx_global_pause`, `tx_global_pause`).

| run (512 B, echoing plain responder) | pause on | pause off |
|---|---|---|
| 2 requester lcores | 36.4 Mpps | 53.5 Mpps |
| 4 requester lcores | 43 Mpps | |

With pause off the echo run is within 2 % of the pure-TX figure; the echoes the
requester cannot absorb are dropped by its NIC (`rx_out_of_buffer`) instead of
pausing the link.

### Receiving the full echo stream

To have the requester also receive everything the responder sends back, it
needs both MPRQ and an RX burst as large as its TX burst. Measured 2026-09-30
with 4 requester lcores at 16 M each (64 Mpps total), 256 B, `--txd 4096`, and a
4-core plain responder that forwarded all 64 Mpps without loss:

| requester settings | NIC buffer discards | RX queue overflow (RX missed) | lost |
|---|---|---|---|
| defaults (`--rx-burst 32`, no MPRQ) | 2.44 Mpps | 2 kpps | 3.4 % |
| `--devargs mprq_en=1,rxqs_min_mprq=1` | 62 pps | 8.04 Mpps | 12.6 % |
| MPRQ plus `--rx-burst 512 --tx-burst 512` | | | no loss reported |

Without MPRQ the requester's NIC cannot write every echo into host memory while
also reading 64 Mpps of TX data. MPRQ removes that, but then the lcores, which
poll only 32 packets per iteration and pay about 7 % more cycles per packet for
MPRQ, cannot drain the queues. A 512-packet RX burst fixes the drain rate:

```sh
scripts/run.sh --role requester --lcores 2,4,6,8 --doca-mode none --musq-group 0 \
  --traffic udp --frame-size 256 --rps 16000000 --warmup 1 --duration 20 \
  --stats-interval 2 --txd 4096 --rx-burst 512 --tx-burst 512 \
  --devargs mprq_en=1,rxqs_min_mprq=1
```

The requester's "Drops (TX-RX)" mixes loss on both hosts. To see where packets
are lost, compare `rx_phy_packets` with `rx_good_packets` and TX on each side:
`rx_prio0_buf_discard_packets` is the NIC failing to DMA into memory,
`rx_out_of_buffer` is the lcores failing to refill the RX queue.

The responder at 256 B without MPRQ forwards about 66 Mpps (134 Gbps) with 4
cores about 80 % busy; above that its NIC discards the excess the same way.

## Requester request/response matching

These runs used `--work header` on the responder and were measured with the
earlier requester build (`--tx-fill copy` behaviour, pause on), so they describe
the RX side, not TX capacity.

**RX/TX asymmetry under overload.** Each loop iteration receives at most
`--rx-burst` (32) packets but sends up to 512. Once the lcore cannot keep up, RX
collapses: at a 30 M target one lcore sent 19.5 Mpps and received 1.2 Mpps, and
the NIC dropped the rest as `rx_out_of_buffer`, which then shows up as Drops and
Req lost. `--rx-burst 512 --tx-burst 512` makes overload graceful (TX self-limits,
loss stays near zero). korba uses the same 32/512 constants.

**Cross-lcore completions and ESP.** RSS hashes ESP on the IP pair only, so
without steering every ESP response lands on one requester lcore, and completing
another lcore's request costs about twice a local one.

| configuration | matched requests/s |
|---|---|
| 1 lcore, UDP 512 B | 12.2 M |
| 1 lcore, ESP 92 B, `--rx-burst 512 --tx-burst 512` | 17.4 M |
| 2 lcores, UDP (half the completions cross-lcore) | 9.9 M each |
| 4 lcores, UDP (three quarters cross-lcore) | 7.4 M each |
| 4 lcores, ESP, `--rx-steer none` | 7.5 M in total |
| 4 lcores, ESP, `--rx-steer flow`, esp_rr responder in `accumulate` | 60 M in total (4 × 15 M, 0.01 % loss) |
| same, `--rx-burst 512 --tx-burst 512` | 69 M in total (0.7 % loss) |

`--rx-steer flow` (the default) gives each requester lcore its own source IP and
installs one ingress `rte_flow` rule per lcore, so every response returns to the
queue of the lcore that owns the request.

**esp_rr responder in `--tx-mode immediate`.** With mu_sq lanes and small receive
bursts (average 2 to 3 packets), the responder itself drops 15 to 37 % on TX (its
"TX drops" row); with bursts of 9 or more it drops about 1 %. `--tx-mode
accumulate` removes the drops (0 % at 48 M requests/s) at the cost of 35 to 70 µs
RTT. Use `accumulate` for ESP throughput runs.

## Responder

### How to measure

```sh
# danger51 (add --devargs mprq_en=1,rxqs_min_mprq=1 for MPRQ)
scripts/run.sh --lcores 2,4,6,8 --doca-mode none --musq-group 0 --tx-mode immediate --stats-interval 2

# danger50: 4 requester lcores offer the most (see "Generator limits")
scripts/run.sh --role requester --lcores 2,4,6,8 --doca-mode none --musq-group 0 \
  --traffic udp --sport 1025-2048 --frame-size 128 --rps 60000000 \
  --tracker-size 65536 --warmup 1 --duration 8 --stats-interval 4
```

Read the responder's port "TX pkts" rate and its per-core "CPU busy %". Packets
the responder cannot take show up in its xstats as
`rx_prio0_buf_discard_packets` (NIC port buffer) or `rx_out_of_buffer` (RX queue
out of descriptors).

### Results

| cores | 128 B | 128 B, MPRQ | 512 B | 512 B, MPRQ |
|---|---|---|---|---|
| 1 | 33.4 Mpps, CPU | 24.4 Mpps, CPU | 24.7 Mpps, one RX queue | |
| 2 | 61.7 Mpps | | 35.3 Mpps | |
| 4 | 98.8 Mpps | | 44.0 Mpps, NIC RX DMA | 51.7 Mpps, generator-bound |
| 8 | 114 Mpps, NIC RX DMA | 152 Mpps, CPU and TX ring | | |
| 12 | | 157 Mpps, NIC | | |

MPRQ means `--devargs mprq_en=1,rxqs_min_mprq=1`.

### What limits it

- **One core at 128 B is CPU-bound at about 69 cycles/packet.** About 72 % of it
  is the mlx5 driver (RX burst, CQE processing and decompression about 46 %, TX
  burst and completions about 25 %); the responder loop itself is 28 %, mostly
  the MAC swap. IPC is 2.9 with no memory misses. `rxq_cqe_comp_en=0` does not
  help (32.9 Mpps on one core, 106 Mpps on eight).
- **One core at 512 B stops at about 25 Mpps (100 Gbps) while 96 % busy**; a
  single RX queue appears to be the cap.
- **Without MPRQ the NIC cannot DMA packets into host memory any faster.**
  Beyond about 114 Mpps at 128 B and 44 Mpps at 512 B the cores go idle while the
  NIC drops the excess as `rx_prio0_buf_discard_packets`, with RX descriptors
  still available.
- **MPRQ raises the NIC ceiling to about 157 Mpps at 128 B but costs about 40 %
  more CPU per packet** (96 vs 69 cycles on one core), because the driver copies
  packets up to 128 B out of the stride buffer. `mprq_max_memcpy_len=0` avoids
  the copy but is slower still (128 cycles). mlx5 only enables MPRQ with at least
  `rxqs_min_mprq` RX queues, 12 by default, and otherwise logs "MPRQ is requested
  but cannot be enabled" and runs without it.

Recommended settings: up to about 100 Mpps at 128 B, 4 cores without MPRQ; for
the maximum at 128 B, 12 cores with MPRQ; at 512 B, 4 cores with MPRQ.

### Generator limits

At 128 B, 4 requester lcores offer about 170 Mpps. Eight offer only about 115 Mpps,
because each requester lcore also receives its share of the echoes. At 512 B the
requester is capped by its own PCIe link at 52 to 55 Mpps, so the responder's
512 B ceiling with MPRQ is at least 51.7 Mpps and was not reached.

### Open issue

In one 4-core 512 B run a single responder TX queue refused every packet for
about 8 s (its "TX drops" grew by about 25 M per 2 s window) while reporting no
TX errors; the other queues were unaffected. A repeat run did not reproduce it.
