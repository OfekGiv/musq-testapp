# mu_sq TX investigation: status 2026-10-01

Where the investigation of mu_sq TX drops on the esp_rr responder stands, what
has been ruled out, the tools added for it, and what to run next. Throughput
ceilings of the requester and the plain RSS responder are in
[throughput.md](throughput.md).

## Host state to restore first

The setup below does not survive a reboot. Check it before any run.

| item | danger50 | danger51 | how to set |
|---|---|---|---|
| 1 GB hugepages | 4 per NUMA node | 4 per NUMA node | `echo 4 \| sudo tee /sys/devices/system/node/node{0,1}/hugepages/hugepages-1048576kB/nr_hugepages` |
| 2 MB hugepages | 2048 per NUMA node | 2048 per NUMA node | same with `hugepages-2048kB` and 2048 |
| 1 GB hugetlbfs mounts | `/mnt/huge-1G`, `/mnt/huge` | same | `sudo mount -t hugetlbfs -o pagesize=1G nodev /mnt/huge-1G` (and `/mnt/huge`) |
| global pause | off | off | `sudo ethtool -A enp23s0f1np1 rx off tx off` |

With pause on, a receiver that falls behind pauses the link and caps the
sender's TX, which makes every loss figure misleading.

danger51 runs a rebuilt mlx5 PMD with per-lane WQ telemetry (see below). The
original library is kept next to it:

```
/homes/ofekdg/prom-dpdk/install-o2-nometa/lib/x86_64-linux-gnu/dpdk/pmds-26.0/
    librte_net_mlx5.so.26.0                        # with /mlx5/txq/lane_wq
    librte_net_mlx5.so.26.0.bak-before-lane-tel    # original, copy back to revert
```

It was built with `ninja -C build-o2-nometa drivers/librte_net_mlx5.so.26.0` and
copied into place. A full `meson install` was avoided because 37 unrelated
targets were pending in that build directory.

## Tools added

- **Responder summary** in every stats interval: RX/TX pps and wire Gbps, the
  rate offered at the port (`rx_phy_packets`), NIC discards
  (`rx_prio0_buf_discard_packets`), RX missed and not-forwarded packets.
- **`/mlx5/txq/lane_wq`** (prom-dpdk, `struct mlx5_txq_lane_tel`, switch
  `MLX5_MU_LANE_TELEMETRY`): each TX queue's own core records, inside its
  `rte_eth_tx_burst`, calls and packets offered and refused, free WQEBBs at each
  resource check (sum, min, latest, log2 histogram), and the WQEBBs a post needs
  (`room_need`). It writes only its own cache-aligned record from values the
  burst already computed, and the telemetry command reads only those records.
  No measurable overhead: 33.37 vs 33.30 Mpps on one CPU-bound core at 128 B.
- **`scripts/musq_lane_wq.py`** prints it per interval:
  `sudo scripts/musq_lane_wq.py -i 1`, or `--raw` for the cumulative counters.
- **`/mlx5/musq/txq_status`** (already in prom-dpdk) adds per-lane `wqe_ci`,
  `wqe_pi`, completion-queue state and slot-fill histograms
  (`mu_slot_hist_*`). It reads live ring state from the telemetry thread, so use
  it for diagnosis, not during benchmarks.

## Findings so far

### 1. mu_sq TX has a lower ceiling than separate queues

esp_rr responder, 4 cores, `--txd 16384 --rxd 4096`, ESP traffic from 4
requester lcores:

| frame | mu_sq, 4 lanes: TX at the limit | refused on TX | without mu_sq (`--musq-group 0`) |
|---|---|---|---|
| 128 B | 71.9 Mpps (73.7 Gbps) | 27 % | not measured (run failed to start) |
| 256 B | 48.0 Mpps (98.4 Gbps) | 30 % | 65.9 Mpps, RX-bound |
| 512 B | 29.9 Mpps (122.8 Gbps) | 37 % | 42.1 Mpps, RX-bound |

Time per packet fits about 7 ns fixed plus 0.05 ns per byte. Without mu_sq the
esp_rr responder is limited on RX, not TX (MPRQ is not enabled in DOCA mode), so
separate queues' TX ceiling in this setup is still unknown; it is at least the
figures shown. At 60 Mpps offered and 256 B, the same load with
`--musq-group 0` forwarded all 60 Mpps with no drops, against 52 Mpps with
mu_sq.

Fewer lanes were faster at 256 B: 2 lanes forwarded 54.2 Mpps with no TX
refusals (CPU-bound at 98 % busy), 4 lanes 48.0 Mpps with 30 % refused.

### 2. The drops are full rings, and on a fresh responder the NIC is the limit

At 60 Mpps offered (256 B, fresh responder), every lane's ring was full
(`used` 16,348 of 16,380 usable WQEBBs) while unread completions stayed at 0
to 29 (`scan_lag`), so software reads completions promptly. Each lane writes its
own doorbell record (`*db_rec = wqe_ci`) on every burst, so the NIC's
executable limit is the minimum `wqe_ci` over the lanes. 600 samples at 10 ms
showed lane skew mostly 0 to 1 rounds (max 6, 162 WQEBBs) while the
fully-posted, not-yet-completed backlog never fell below 16,204 WQEBBs, about
1 ms of work. So in that run a lagging lane did not starve the NIC.

A mu_sq post needs `room_need` = (9 << log_group_size) + lap pad = 40 WQEBBs
for 4 lanes, not one slot: posting advances the lane by a whole round. Lanes
refuse with 36 free. In the 60 Mpps run `no-slot%` equalled `refused%` on every
lane.

### 3. Lanes drift apart over a responder's lifetime

On a responder that has run through several traffic runs, lanes drift far
apart. At 10 Mpps per lane:

| lane | received | posted to TX | refused | tx_burst calls | slots ahead of lane 3 | min free WQEBBs |
|---|---|---|---|---|---|---|
| 0 | 100,280,044 | 100,278,332 | 1,712 | 3,188,371 | 430 | 36 |
| 1 | 100,280,042 | 100,280,042 | 0 | 3,188,073 | 187 | 7,632 |
| 2 | 100,280,042 | 100,280,042 | 0 | 3,187,999 | 113 | 11,704 |
| 3 | 100,280,040 | 100,280,040 | 0 | 3,187,886 | 0 | 12,856 |

- **Steering is balanced**: RX per lane is equal within 4 packets in 100 M
  (`SN & 3` round-robin).
- **Slot counts are not.** Each slot advances a lane one round (36 WQEBBs), so
  a lane that closes more short slots runs ahead. The NIC completes only up to
  the slowest lane, so the lanes that are ahead hold the backlog: lane 0 reached
  36 free and refused 1,712 packets at a load far below the NIC's limit.
- **The skew never shrinks.** With traffic stopped it stays frozen, and the
  extra slots of the lanes that are ahead (about 11,000 packets on lane 0 in an
  earlier run) are posted but not executed until the other lanes post as many
  slots. At 10 Mpps per lane, 363 slots of skew is about 1.1 ms extra latency on
  the leading lane. A lane 455 rounds ahead (the whole 16,380-WQEBB ring) cannot
  post at all.
- **The order is systematic**: in two separate runs lane 0 was furthest ahead,
  then 1, 2, 3.

### 4. Likely cause of the drift: the responder's drain timer (not yet tested)

In `--tx-mode accumulate` each lane flushes its partial TX buffer every
`--tx-drain-us` (100 µs), and `prev_drain` is reset only when the timer fires,
not when a full 32-packet buffer is sent ([responder.cpp](../src/responder.cpp),
the `flush_txbuf` call at the top of the loop). So under continuous traffic
every lane forces a short slot about every 100 µs whenever its buffer is
non-empty, with its own timer phase. With `SN & 3` a lower lane receives its
packet earlier in every group of four sequence numbers, so it is more often
non-empty when its timer fires. Each lane showed about 100,000 short slots,
differing by a few hundred between lanes, which matches the skew.

## Design constraints agreed

- Any fix must not add cross-core memory traffic: no reading of sibling lanes'
  indices or counters, no group-shared counters in the hot path.
- Proposed lane-local fix in the PMD (not implemented): split time into epochs
  on the common TSC grid (for example 100 µs); between epoch boundaries close
  only full slots; at each boundary close the partial slot and post NOP slots
  until the lane's slot count equals `floor(packets / 32) + epoch`. Every lane
  computes the same target from its own packet count, which round-robin steering
  keeps equal, so skew stays within about one round. Costs up to one partial and
  a few NOP slots per lane per epoch; needs balanced steering; the app cannot
  post NOP slots, so it has to live in the PMD.
- App-only mitigation: reset `prev_drain` on every flush, so the timer fires
  only after a real traffic pause. Continuous traffic then produces only full
  slots. It does not bound skew from real idle gaps.

## Next steps

1. **Test the drain-timer hypothesis.** Fresh responder each time, requester
   `--traffic esp --frame-size 256 --rps 10000000` on 4 lcores for about 20 s,
   then compare per-lane `calls` (`sudo scripts/musq_lane_wq.py --raw`):

   ```sh
   # danger51, one at a time
   scripts/run.sh --lcores 2,4,6,8 --doca-mode esp_rr --stats-interval 2 --txd 16384 --rxd 4096
   scripts/run.sh --lcores 2,4,6,8 --doca-mode esp_rr --stats-interval 2 --txd 16384 --rxd 4096 --tx-drain-us 100000
   scripts/run.sh --lcores 2,4,6,8 --doca-mode esp_rr --stats-interval 2 --txd 16384 --rxd 4096 --tx-mode immediate

   # danger50
   scripts/run.sh --role requester --lcores 2,4,6,8 --doca-mode none --musq-group 0 \
     --traffic esp --frame-size 256 --rps 10000000 --warmup 1 --duration 20 --stats-interval 2
   ```

   Expect the 100 ms drain run to show far less skew if the timer is the cause.
2. **Restart the responder before every measurement** until the drift is fixed;
   skew only resets when the queues are recreated.
3. If confirmed, implement the `prev_drain` reset in the responder, then decide
   on the PMD epoch padding.
4. **Isolate the NIC's mu_sq TX ceiling** with a TX-only test: run the requester
   on danger51 (prom-dpdk PMD, mu_sq) to a dummy MAC, sweeping group size 1, 2, 4
   and frame size. Needs the requester's `--musq-group 0` restriction relaxed;
   danger50's stock PMD has no mu_sq.
5. Optional: rebuild prom-dpdk with `MLX5_MU_DIAG_COUNTERS 1`
   (`drivers/net/mlx5/mlx5.h`) for refusal and doorbell counters per call.
