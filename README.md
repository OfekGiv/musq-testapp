# musq-testapp

Receive-then-send-back DPDK test application for the mu_sq ("PROM", multi-user
send queue) feature of the mlx5 PMD fork in `/homes/ofekdg/prom-dpdk` and its
DOCA Flow steering pipelines. It replaces the modified `examples/l3fwd` used so
far: every packet received on the single port is sent back on the same port,
after an optional, configurable amount of mock per-packet work that emulates
core busyness.

Modelled on korba's responder (`/homes/ofekdg/korba/src/responder.cpp`):
command-line configuration, DPDK port counters + xstats and per-core CPU
busy/idle reported every interval and at exit.

Setup: DUT `danger51`, NIC `0000:17:00.1` (ConnectX-9, NUMA 0 = even CPUs).
Traffic generator: TRex on `danger50` (profiles in `/homes/ofekdg/trex_scripts/`).

## Build

Prerequisites: a prom-dpdk **shared-library** build that was `ninja install`ed
(default `build-o2-nometa` / `install-o2-nometa`), which also produced the DOCA
pipelines SDK static library `<build>/doca_pipelines_sdk/libdoca_pipelines_meta_rr.a`.
`fmt` comes from the system; `cxxopts` is vendored in `third_party/`.

```sh
meson setup build -Dpkg_config_path=/homes/ofekdg/prom-dpdk/install-o2-nometa/lib/x86_64-linux-gnu/pkgconfig
ninja -C build
# second build WITHOUT the DOCA libraries, required for --doca-mode none (plain RSS)
meson setup build-nodoca -Denable_doca=disabled -Dpkg_config_path=/homes/ofekdg/prom-dpdk/install-o2-nometa/lib/x86_64-linux-gnu/pkgconfig
ninja -C build-nodoca
```

Why two builds: with the DOCA libraries loaded into the process, the mlx5 PMD
delivers nothing to the RX queues in plain (non-DOCA) mode -- packets show up in
`rx_phy_packets` / `rx_unicast_packets` but never in a queue -- even though no
DOCA API is called (verified 2026-09-25 against testpmd from the same tree and
against a no-DOCA build of this app, which both receive everything). The DOCA
build therefore refuses `--doca-mode none`, and `scripts/run.sh` runs
`build-nodoca/musq-testapp` automatically when that mode is requested.

One packet per WQE (`--pkts-per-wqe 1`) needs its own binaries: the mu_sq slot
size is a compile-time constant of the mlx5 PMD (`MLX5_MU_EMPW_PKTS`, 32 packets
= 9 WQEBBs by default; 1 packet = 1 WQEBB), so a second prom-dpdk is built and
installed with `-DMLX5_MU_EMPW_PKTS=1`, and the app is built against it.
`scripts/run.sh` picks `build-ppw1` / `build-nodoca-ppw1` and that install when
it sees `--pkts-per-wqe 1`.

```sh
cd /homes/ofekdg/prom-dpdk
meson setup build-o2-nometa-ppw1 -Ddefault_library=shared -Dexamples=l3fwd -Denable_trace_fp=false \
  -Dbuildtype=debugoptimized -Dprefix=$PWD/install-o2-nometa-ppw1 -Dc_args=-DMLX5_MU_EMPW_PKTS=1
ninja -C build-o2-nometa-ppw1 install
cd /homes/ofekdg/musq-testapp
P=/homes/ofekdg/prom-dpdk
meson setup build-ppw1 -Dpkg_config_path=$P/install-o2-nometa-ppw1/lib/x86_64-linux-gnu/pkgconfig \
  -Dprom_install_dir=$P/install-o2-nometa-ppw1 -Dprom_build_dir=$P/build-o2-nometa-ppw1
meson setup build-nodoca-ppw1 -Denable_doca=disabled -Dpkg_config_path=$P/install-o2-nometa-ppw1/lib/x86_64-linux-gnu/pkgconfig \
  -Dprom_install_dir=$P/install-o2-nometa-ppw1 -Dprom_build_dir=$P/build-o2-nometa-ppw1
ninja -C build-ppw1 && ninja -C build-nodoca-ppw1
```

A copy of the install directory is not enough: EAL has its plugin directory
compiled in and would load the 32-packet PMD as well.

Since 2026-10-31 the prom-dpdk PMD forms one mu_sq group per `mu_sq_log_grp_size`
consecutive TX queues (several groups per port, see "Several groups"); the
installed PMDs of both variants have it (the previous libraries are kept as
`librte_net_mlx5.so.26.0.bak-before-groups`), and the app refuses
`--musq-groups` > 1 on an older PMD.

With mu_sq a 1-packet slot is posted as an Enhanced-MPW WQE that holds one
packet. `--devargs txq_mpw_en=0` posts it as a SEND WQE instead (same three
segments in one WQEBB, opcode SEND, and the SQ is created without multi-packet
WQEs); lanes, completions and the doorbell position are the same. The startup
line `port 0 TX burst mode:` shows which one runs: `Enhanced MPW` or `No MPW`.

```sh
scripts/run.sh --lcores 2,4 --doca-mode esp_rr --pkts-per-wqe 1 --txd 8192 --rxd 4096 --devargs txq_mpw_en=0
```

To link against another prom-dpdk build, pass matching options:

```sh
meson setup build-dbg \
  -Dpkg_config_path=/homes/ofekdg/prom-dpdk/install-w-debug/lib/x86_64-linux-gnu/pkgconfig \
  -Dprom_install_dir=/homes/ofekdg/prom-dpdk/install-w-debug \
  -Dprom_build_dir=/homes/ofekdg/prom-dpdk/build-doca-w-debug
```

Options (`meson_options.txt`): `prom_install_dir`, `prom_build_dir`,
`doca_sdk_root`, `dpdk_pmd_subdir`, `enable_doca`. The binary carries a
DT_RPATH to the prom-dpdk libs, its PMD dir and the vendored DOCA bundle, so the
system DPDK in `/opt/mellanox/dpdk` (same sonames) is never picked up. Check with
`ldd build/musq-testapp | grep -E 'rte_|doca'`. A static DPDK build does not work
(the DOCA libraries link shared DPDK). Because of that RPATH, `PROM_INSTALL_DIR`
in the environment of `scripts/run.sh` does not change which libraries load.

### Release builds for lowest cycles per packet

`install-o2-nometa*` are `-O2` debugoptimized builds. `scripts/build_prom_o3.sh`
builds the prom-dpdk tree out of tree (the tree is only read) at `-O3` release,
into `/homes/ofekdg/dpdk-eval/prom-<variant>` (not under `/tmp`: EAL refuses to
load drivers from below a world-writable directory), and the app dirs linked to
each: `o3` (32-packet slots), `o3-ppw1` (1-packet slots), and `o3nt`,
`o3nt-ppw1` without lane telemetry (no `/mlx5/txq/lane_wq`, no doorbell rows).
Rerun it after changing the PMD. `MUSQ_BUILD` picks them in `scripts/run.sh`:

```sh
scripts/build_prom_o3.sh                    # all four, about 5 minutes
MUSQ_BUILD=o3 scripts/run.sh --lcores 2,4 --doca-mode esp_rr --txd 8192 --rxd 1024
```

Measured on 2026-10-17/18 (danger51, 64 B, 2.4 GHz, CPU-bound): without mu_sq,
`-O3` saves 2 cycles/pkt and dropping telemetry 1 more (stock v25.11 is 2
lower still, in `mlx5_tx_burst_none_empw`); with mu_sq (group 2) neither
changes the cycles. `--rxd 1024` instead of 4096 saves about 5 cycles/pkt under
mu_sq (fewer cold mbufs) at the price of less room for lane stalls; keep
`--txd 8192`, smaller TX rings bring back ring-full refusals.

## Run

`scripts/run.sh` runs the binary under `sudo env -i` with the right
`LD_LIBRARY_PATH` (same shape as the prom-dpdk readme). Env overrides:
`PROM_INSTALL_DIR`, `DOCA_SDK_ROOT`, `MUSQ_BIN`, `MUSQ_ENV` (extra vars).

```sh
# 4 mu_sq lanes, ESP sequence-number round robin, 3 us of mock work per packet
scripts/run.sh --lcores 2,4,6,8 --doca-mode esp_rr --work fixed:3000 --stats-interval 5

# plain RSS baseline, no mu_sq, no DOCA (run.sh switches to build-nodoca/)
scripts/run.sh --lcores 2,4,6,8 --doca-mode none --musq-group 0

# one core polling two RSS queues (like testpmd --rxq=2 --nb-cores=1)
scripts/run.sh --lcores 20 --doca-mode none --musq-group 0 --rx-queues 2

# show the resolved config and EAL argv without touching the NIC
build/musq-testapp --lcores 2,4 --dry-run
```

Stop with Ctrl-C (a second signal kills), or `--duration <s>`. A final report is
always printed. The line `entering main loop on lcore N` (one per worker) is the
ready signal the TRex harness waits for; it needs `--log-level 7` (the default).

### Options

| option | default | meaning |
|---|---|---|
| `--pci` | `0000:17:00.1` | data-path PF |
| `--devargs` | | extra mlx5 devargs, appended after `mu_sq_log_grp_size` |
| `--musq-group` | `auto` | mu_sq group size (lanes per group): `auto` = TX queues / `--musq-groups` (workers × `--tx-lanes` with one group), `0` = off, or a power of 2 ≤ 128. With one worker and `--tx-lanes 1`, a group of several lanes is driven by that worker's single TX queue (see "One core, two lanes") |
| `--musq-groups` | 1 | mu_sq groups on the port, each its own shared TX ring (power of 2 ≤ 64): group g = TX queues g·N..g·N+N−1 with N = `--musq-group` lanes, so `--lcores` is G × N lcores with one RX and one TX queue each. In a DOCA mode the NIC hashes a flow (outer IPv4 src + dst) to one group and round-robins inside it (`esp_rr` only). Needs the prom-dpdk PMD with multi-group mu_sq (see "Several groups") |
| `--hash-probe` | | responder, DOCA mode with `--musq-groups` > 1: `<src-ip>[+n]:<dst-ip>`. Logs at start which group each of the n flows src-ip, src-ip+1, .. → dst-ip lands in (the requester sends from `--src-ip` + lcore index), e.g. `1.1.1.1+8:2.2.2.2` |
| `--tx-lanes` | 1 | responder, one worker lcore: TX queues (mu_sq lanes) that lcore posts to, dealing the packets to them in turn. A power of 2 ≤ 8 |
| `--rx-queues` | 1 | responder, `--doca-mode none`: RX queues per worker lcore, one `rte_eth_rx_burst` from each in turn; RSS spreads the traffic over all workers' queues, and the port report shows each queue's share. TX is unchanged (the worker's TX queue, or `--tx-lanes`). At most 8 |
| `--db-wqes` | 0 | responder with mu_sq: WQEs a lane posts per doorbell. The PMD then rings the lane's doorbell every that many WQEs (mu_sq slots), wherever a burst ends, instead of once per send pass (mlx5 devarg `mu_sq_db_wqes`; at most half the slots a lane has in the ring). `0` keeps the PMD's rule. A batch of N costs latency: a lane's packets wait up to N WQEs at that lane's rate (with 128 and 2 Mpps per lane, RTT p50 53 us against 9 with the PMD's rule, finding 18); keep `0` when latency matters. See "Doorbell rate" |
| `--masters` | | responder: software distributor, the master lcores (see "Software distributor"); `--lcores` are then the workers, split evenly among them, with no NIC queues of their own |
| `--tx-masters` | | `--masters`: one lcore per master (paired in lcore order) that takes over its collecting and sending half, a two-stage pipeline: the master then only receives and deals, the TX master collects the workers' returns in order and sends them on the master's TX queue (see "Software distributor") |
| `--dist-order` | `rr` | `--masters`: `rr` (deal and collect round robin, implicit order) or `seqn` (sequence numbers kept by the master and a reorder buffer) |
| `--dist-deal` | `rr` | `--masters`: worker of the next chunk: `rr`, or `lb` (fewest packets in flight; needs `--dist-order seqn`) |
| `--dist-chunk` | 32 | `--masters`: consecutive packets dealt to one worker before moving on (1 = per-packet round robin; 32 = whole bursts, far fewer ring operations per packet; the order is kept either way) |
| `--dist-ring` | 2048 | `--masters`: entries of each master ↔ worker ring (power of 2 ≥ 4 × rx-burst); at most ring − rx-burst packets in flight per worker, beyond that the master stops receiving |
| `--dist-self` | 0 | `--masters`: the master processes up to this many packets per deal cycle itself (one more slot in the round robin, after the workers) whenever a worker is behind (`--dist-self-backlog`) and the master has had idle passes lately (`--dist-self-idle`); 0 = off (see "Software distributor") |
| `--dist-self-backlog` | 128 | `--dist-self`: tokens waiting in some worker's input ring from which the master takes its share |
| `--dist-self-idle` | 5 | `--dist-self`: the master takes a share only when at least this percent of its recent loop passes (windows of 1024) were idle; 0 = whenever a worker is behind |
| `-l, --lcores` | required | worker lcores (`2,4,6,8` or `2-8`); one RX + one TX queue each, queue id = worker index (with `--masters`: no queues, see above) |
| `--main-lcore` | `0` | EAL main lcore (stats only) |
| `--doca-mode` | `esp_rr` | `none` / `meta_rr` (IPv4+UDP) / `esp_rr` (IPv4+ESP, steer on ESP SN). Both pipelines drop non-matching traffic in the NIC |
| `--doca-group` | `1` | steering group: consecutive packets per queue before moving on (was `L3FWD_DOCA_GROUP`) |
| `--rx-burst` / `--tx-burst` | 32 / 32 | tx a power of 2; rx ≤ tx, except for a responder in accumulate mode, which splits larger RX bursts into tx-burst sends (e.g. `--rx-burst 64 --tx-burst 32` under 32-packet mu_sq slots). Under mu_sq one `rte_eth_tx_burst` call ≤ tx-burst = one eMPW slot |
| `--tx-mode` | `accumulate` | `accumulate`: buffer to tx-burst and flush (l3fwd behaviour); `immediate`: send each RX burst as is |
| `--tx-drain-us` | 100 | accumulate-mode drain timer (was `L3FWD_TX_DRAIN_US`) |
| `--work` | `fixed:0` | mock work per packet: `fixed:<ns>`, `bimodal:<lo_ns>/<hi_ns>/<pct_hi>`, `exp:<mean_ns>`, `header` (the request header carries the time in µs; use with the requester role) |
| `--swap` | `l2l3` | header rewrite before send-back: `l2l3` (MACs + IPv4 + UDP ports), `l2`, `none` |
| `--rxd` / `--txd` | 1024 / 1024 | descriptors per queue (TX descriptors are effectively shared by a mu_sq group) |
| `--shared-pool` | off | one mempool for all workers instead of one per worker lcore |
| `--no-promisc` | off | |
| `--duration` / `--warmup` / `--stats-interval` | 0 / 0 / 1 s | |
| `--stats-csv` | | append long-format CSV rows (`scope,id,metric,total,delta,rate` per window) |
| `--xstats` | `filtered` | `filtered` (tx_/rx_/phy/discard/drop/missed/nombuf/mu_), `all`, `final` (only in the final report) |
| `--pkts-per-wqe` | 32 | `32`: multi-packet WQEs (mu_sq 32-packet slots of 9 WQEBBs, or eMPW without mu_sq). `1`: one packet per WQE. With mu_sq that is a 1-packet slot of 1 WQEBB (lane step = group size) and needs the 1-packet PMD build, which `scripts/run.sh` selects; the app checks the PMD's slot size at start. Without mu_sq the app adds `txq_mpw_en=0` (one SEND WQE per packet); with mu_sq add `--devargs txq_mpw_en=0` for SEND WQEs in the slots |
| `--lane-epoch-us` | 0 (off) | responder: each lane (core) records its cumulative RX and TX packet counts and its busy and idle CPU cycles (the CSV also has `busy_pct`, the lane's CPU busy % in that epoch), and in the DOCA build also its `wqe_ci` / `wqe_pi` / free WQEBBs and slot counters and its CQ state (`cq_ci`, `cq_pi`, `cq_free`, `cq_done`, see below) and the doorbells the PMD has rung (`doorbells`, cumulative), once per epoch of this many µs on a TSC grid common to all lanes; no cross-core access. Dumped at exit; `scripts/lane_epoch_report.py` reports RX packets per lane per epoch and their balance across lanes, CPU busy % per lane and per epoch, plus lane skew when the WQ columns are present |
| `--lane-epoch-out` / `--lane-epoch-max` | `lane_epoch.csv` / 120000 | CSV file and records per lane (96 B each) for `--lane-epoch-us` |
| `--file-prefix` | `musq` | EAL file prefix (distinct from l3fwd's `rte`) |
| `--log-level` | 7 | EAL log level (7 = INFO) |
| `--eal-args` | | extra EAL args, e.g. `"--trace=pmd.net.mlx5.tx.lcore --trace-dir=/tmp/t"` |

Mapping from the l3fwd env knobs: `L3FWD_BUSY_WAIT_NS=3000` → `--work fixed:3000`,
`L3FWD_DOCA_GROUP` → `--doca-group`, `L3FWD_TX_DRAIN_US` → `--tx-drain-us`,
`DOCA_PIPELINES_MODE` → `--doca-mode` (the app sets the env var itself),
`--queues N` → implied by the number of `--lcores`, `--per-lcore-pool` → default.

### One core, two lanes

Two ways to run a mu_sq group of two lanes from a single lcore, with one RX
queue (the round-robin steering then has one target, so every packet goes to
that core). Both forward the packets in arrival order.

```sh
# 1. One data path structure: one TX queue (one mlx5_txq_data) posts every slot
#    of the ring, lane 0's and lane 1's in turn, and rings both lanes' doorbells.
scripts/run.sh --lcores 2 --doca-mode esp_rr --pkts-per-wqe 1 --txd 8192 --rxd 4096 --musq-group 2

# 2. Two data path structures: two TX queues, one per lane, as two cores would
#    have. The lcore deals packet i to lane i % 2; each lane has its own buffer
#    of --tx-burst packets and its own rte_eth_tx_burst calls and doorbells.
scripts/run.sh --lcores 2 --doca-mode esp_rr --pkts-per-wqe 1 --txd 8192 --rxd 4096 --tx-lanes 2
```

The first needs the prom-dpdk mlx5 PMD: a port with one TX queue and
`mu_sq_log_grp_size` > 0 makes that queue drive the whole group (`mu_log_stride`
= 0 and `mu_db_lanes` in `struct mlx5_txq_data`; the PMD logs "Tx queue 0 alone
drives all 2 lanes"). One `rte_eth_tx_burst` call of 32 packets there fills 16
rows and rings two doorbells; in the second, each lane rings one doorbell per 32
of its own packets, i.e. two doorbells per 64 packets. `--tx-burst 64` in the
first gives the same doorbell rate.

In the per-epoch lane log the second has one record per TX queue: `tx_pkts`,
`tx_drops` and the WQ columns are the lane's, `rx_pkts` and the CPU cycles are
the lcore's and so appear in both lanes' records.

### Several groups (`--musq-groups`)

One shared send queue stops at about 44 M one-packet WQEs per second (finding 22
in [docs/musq-investigation.md](docs/musq-investigation.md)). `--musq-groups G`
puts G mu_sq groups on the port, each its own shared ring: group g is TX queues
g·N .. g·N+N−1 (N = `--musq-group` lanes), with master TX queue g·N owning the
DevX SQ (N consecutive SQNs), the CQ and the doorbell records that the group's
other lanes attach to. The PMD derives G from the TX queue count and the group
size (`mu_sq_log_grp_size`), so `--lcores` must be G × N lcores with one RX and
one TX queue each; RX queue i and TX queue i belong to the same lcore, so the
queue a packet is steered to is the lane that sends it back.

A flow must stay on one ring to keep its order, and a group keeps order only
because lane l receives exactly every N-th packet of *that group's* stream. The
group decision is therefore taken in the NIC, ahead of the round robin: the
`esp_rr` pipeline gets a `DOCA_FLOW_PIPE_HASH` stage between its root and the
per-group STEER pipes, hashing the outer IPv4 source and destination (the
NIC's own hash function, as RSS would) to one of G entries, each forwarding into
that group's STEER pipe, which round-robins on the ESP sequence-number bits over
the group's N queues as before. No core ever chooses a group: reading a hash from
the mbuf and re-dispatching would leave holes in the other groups' rings and
stall them. `--doca-mode none` with several groups is accepted with a warning
(plain RSS puts a flow on one lane, so nothing is round-robinned inside a group:
a PMD smoke test only); `meta_rr` refuses it.

```sh
# 2 groups x 2 lanes, 1 packet per WQE; which group do the requester's flows land in?
scripts/run.sh --lcores 20,22,24,26 --doca-mode esp_rr --pkts-per-wqe 1 --musq-groups 2 \
  --txd 2048 --rxd 1024 --devargs mu_sq_comp_n=128 --db-wqes 128 --hash-probe 1.1.1.1+8:2.2.2.2
#   mlx5_net: Port 0 mu_sq group 0: master txq 0, lanes txq 0..1, SQN 0x1fc0..0x1fc1, CQN 0x2249, ...
#   mlx5_net: Port 0 mu_sq group 1: master txq 2, lanes txq 2..3, SQN 0x2000..0x2001, CQN 0x224a, ...
#   MUSQ: hash probe: 1.1.1.1 -> 2.2.2.2 lands in group 0 (RX queues 0..1)
#   MUSQ: hash probe: 1.1.1.2 -> 2.2.2.2 lands in group 1 (RX queues 2..3)
# danger50: one flow per group, one SA (own SPI and NIC sequence-number counter) per lcore
scripts/run.sh --role requester --lcores 2,4 --doca-mode none --musq-group 0 --traffic esp \
  --esp-encap hw --frame-size 128 --src-ips 1.1.1.1,1.1.1.2 --rps 2000000 --warmup 1 --duration 10
```

`--hash-probe` evaluates the hash pipe's own function in software
(`doca_flow_pipe_calc_hash`) for the given source IPs; it predicts the hardware
exactly (checked against the per-group counters `esp_rr_hash_g<g>` the pipeline
logs at shutdown). With a hash, G flows land in G distinct groups only by luck
(G!/G^G), so pick the requester's `--src-ips` from the probe. The responder shows
the split in its `RX q<N> pkts` rows (the hash across groups, the round robin
inside each group even to one packet) and a `Group` row (`g0 lane1`) in the
per-core table; the lane-epoch CSV ends every row with the lane's `group` and
carries `groups=` and `grp_size=` in its header. At start the app checks that
the loaded PMD formed G groups (`rte_pmd_mlx5_txq_mu_group`; an older PMD is
refused) and logs each group's queues and SQNs.

**Order inside a group follows each SA's own sequence numbers.** The round robin
is `SN mod N`, so a group preserves a flow's order only while it receives one SN
sequence; two SAs hashed into the same group interleave arbitrarily on the
lanes (finding 9). That is why the requester's hw encap now gives every lcore
its own SA by default (`--esp-sa lcore`): the NIC numbers each lcore's packets
0, 1, 2, .. in the order they leave, so one requester lcore is one SA is one
flow. The SPI and SN of ESP are clear text, also when the payload is encrypted,
so the requester checks on every response that its lcore's SNs increase ("ESP SN
reorders", "ESP SN max back", "ESP SN gap pkts", "ESP foreign SPI"); with one
SA per lcore and the responses on the owner's queue (`--rx-steer flow`, the
default) this is the order proof at full rate. Measured 2026-10-31 with the 2 × 2
responder above at 2 Mpps per lcore, 6 M responses per lcore:

| requester | groups the flows land in | ESP SN reorders | max back | responder TX drops |
|---|---|---|---|---|
| `--src-ips 1.1.1.1,1.1.1.2`, one SA each | 0 and 1 | 0 | 0 | 0 |
| `--src-ips 1.1.1.1,1.1.1.3`, one SA each | both 0 | 12.4 % | 1 (adjacent swaps) | 0 |
| `--esp-sa port`, `--src-ips 1.1.1.1,1.1.1.2` | 0 and 1 | 49 % | 4,879 | 361 k per group |

The last row is the one to avoid: one port-wide SN stream split by the hash gives
every group a random subsequence, `SN mod N` no longer alternates the lanes, the
lanes drift apart without bound (RX q0 4.20 M against q1 3.83 M) and the leading
lane refuses packets. Several SAs in one group only cost bounded skew (the second
row: no drops), so a throughput test may run more flows than groups.

### Software distributor: master and worker cores (`--masters`)

The software alternative to mu_sq, for the comparison. A *master* lcore receives
from its own RX queue(s), deals the packets to *worker* lcores over rings, takes
them back, restores the arrival order and sends them on its own TX queue; the
workers only do the per-packet work (`--work`, `--swap`). `--lcores` stays the
worker list, split evenly among the masters in lcore order; `--main-lcore` is
still only the stats lcore. Master *m* owns RX queues `m*rx-queues..` and TX
queue *m*; with several masters RSS keeps a flow on one master. No mu_sq
(`--musq-group` auto = 0 here), so it runs from the no-DOCA build with
`--doca-mode none` (a DOCA pipeline would spread one flow round robin over the
masters' queues, so with a DOCA mode only one master with one RX queue is
accepted).

```sh
# 1 master (lcore 2) + 2 workers, implicit order; 64-packet bursts are the master's best setting
scripts/run.sh --masters 2 --lcores 4,6 --doca-mode none --musq-group 0 --rx-burst 64 --tx-burst 64
# the same with sequence numbers, least-loaded dealing and chunks of 8 packets
scripts/run.sh --masters 2 --lcores 4,6 --doca-mode none --musq-group 0 --dist-order seqn --dist-deal lb --dist-chunk 8
# two masters with three workers each; RSS splits the flows between the masters
scripts/run.sh --masters 2,10 --lcores 4,6,8,12,14,16 --doca-mode none --musq-group 0
```

Two ways to restore the order, `--dist-order`:

- `rr` (default): the master deals round robin, `--dist-chunk` consecutive
  packets per worker (32 by default: whole bursts; 1 = per packet, like 1-packet
  mu_sq slots behind DOCA round-robin steering, at about 30 more master cycles
  per packet), and collects in exactly the same sequence: a collect
  cursor mirrors the deal cursor, so the order is implicit and no sequence
  number is kept anywhere. The collector stops at the first worker whose next
  packet is not back yet (`Collect stalls`) and resumes there on the next pass:
  a slow worker holds the stream, as a lagging mu_sq lane does.
- `seqn`: the master numbers the packets and keeps each number in a FIFO of its
  own for the worker it dealt the packet to (workers hand packets back in the
  order they got them, so the k-th packet back has the k-th number; nothing is
  written into the mbuf). Returns go into a reorder buffer by number and leave
  from its head. The worker for the next chunk is then chosen round robin
  (`--dist-deal rr`) or as the one with the fewest packets in flight (`lb`).
  Costs a few master cycles per packet more than `rr`; `--dist-deal lb` needs it.

What crosses the rings is a token per packet, the frame's address and length,
not the mbuf: the mbuf pointers stay in a master-local FIFO per worker (indexed
by that worker's dealt and collected counts), so no worker ever reads an mbuf
and the mbuf cache lines are never shared between cores (a worker reading them
makes the master pay a cross-core invalidation on every RX refill). A worker
hands the tokens back in the order it got them; the master only counts them and
takes the mbufs from its FIFO.

Flow control needs no shared counters: the master knows what it dealt to and
collected from each worker and never has more than `--dist-ring` − `--rx-burst`
packets in flight per worker, so neither ring can fill and no enqueue can fail
(`Ring retries` must stay 0). When a worker is that far behind, the master stops
receiving (`RX held passes`) and the NIC's RX ring absorbs and then drops: an
overloaded distributor shows as **RX missed** in the responder summary, where an
overloaded mu_sq lane shows TX drops. The rings are the only memory a master
and a worker share; of the packet itself only the header line crosses cores
(the worker rewrites it, the NIC reads it).

Measured 2026-10-25 (danger51, 256 B ESP frames, 64 Mpps offered, saturation):
a master with 1, 3 or 7 workers forwards 31.6 Mpps at 75 cycles per packet with
`--rx-burst 64 --tx-burst 64` and the default chunk of 32, the same as one
run-to-completion core alone (31.4 Mpps, 73 cycles); per-packet dealing
(`--dist-chunk 1`) costs 101 to 136 cycles depending on the worker count (ring
operations per pass grow with it), `--dist-order seqn` 94 cycles with the tuned
settings. The master is the limit whatever the worker count; its ceiling is one
core's RX + TX cost. Details: `docs/results/swdist-cores-frames-2026-10-25/`.
Against mu_sq and plain RSS at equal total cores with per-packet work
(2026-10-27, finding 18, `docs/results/musq-vs-swdist-work-2026-10-27/`): mu_sq
leads the distributor by the master core it saves (about 2x at 2 cores, 2 to
10 % at 8 cores with 100 ns or more of work per packet, 13 to 26 % without
work), both are 37 to 55 % behind plain RSS without work and within 3 to 6 % of
it from 200 ns of work on; latency is equal to mu_sq's (p50 8 to 13 us) without
work, and with 1 us of work the chunk of 32 costs p50 16 to 30 us unless
`--dist-chunk 1` is used.

**The master works too, `--dist-self`.** With per-packet work the workers
saturate while the master idles (22 % busy at 800 ns of work per packet with
seven workers), and the distributor is simply N−1 cores of work against mu_sq's
N. `--dist-self <n>` makes the master one more slot in its own round robin,
after the workers: when the deal cursor reaches that slot the master keeps the
next n packets of the stream for itself if some worker is behind (at least
`--dist-self-backlog` tokens, 128, waiting in its input ring; returns waiting
for the collector do not count) **and** the master itself had idle passes
lately (at least `--dist-self-idle` percent, 5, of the last 1024 passes),
and none otherwise. It processes them in place right after the burst is
dealt (the workers already have theirs) and publishes their count; with `rr`
order the length it chose for each cycle is recorded for the collector, which
walks the same cycle and takes the master's packets at that slot as it would
a worker's returns. The share is self-adjusting: while the workers keep up
the master takes nothing and the path is unchanged; while the master is the
wall (100 % busy) the idle gate keeps it from taking work it has no cycles
for; in between it takes a chunk per cycle until the backlog clears or its
idle share is used up. Works with `seqn` and `lb` too (the self slot is then
one more candidate). A chunk of n packets at w ns each holds the master's
loop for n × w (32 µs at 1 µs of work), so keep n small against the RX ring.
Measured 2026-10-27 (finding 20, `docs/results/swdist-best-2026-10-27/`):
with `--dist-self 32` the distributor equals mu_sq within ±2 % wherever the
cores are the limit (from 50 ns of work at 2 cores, 100 ns at 4, 200 ns at
8; the master takes up to half of the packets at 2 cores), and stays behind
only at the master's wall (12 to 20 % without work). Latency is unchanged.

**Split pipeline, `--tx-masters`.** The master's ceiling is one core's RX plus
TX cost (about 35 Mpps). `--tx-masters` gives each master a second lcore: the
master (now the RX master) only receives and deals, its TX master collects the
returns in arrival order and sends them on the master's TX queue. The two
halves share the per-worker FIFOs (the dealer writes an entry before the token
enters the ring, the collector reads it after the token comes back out, so the
rings' own release/acquire order carries the writes across) and one counter
per worker, the collector's collected count, which the dealer reads only when
its last-seen copy says a worker is at the in-flight limit (with `seqn` also
the reorder buffer's head number). Everything else stays in one half's cache
lines. What now does cross cores is the mbuf itself: the RX PMD writes it on
the RX master, the TX PMD reads and frees it on the TX master, and it returns
to the RX master's pool cache through the mempool ring. Measured 2026-10-27
(finding 19, `docs/results/swdist-split-2026-10-27/`): that hand-over costs
25 cycles per packet unloaded and 60 at the wall, so the RX master alone costs
what the whole master did (70 against 69 cycles) and the split forwards the
same 34 Mpps at 128 B with one worker fewer: no gain, kept as the measured
answer to "split the master".

```sh
# RX master lcore 2, TX master lcore 4, six workers: 8 cores in all
scripts/run.sh --masters 2 --tx-masters 4 --lcores 6,8,10,12,14,16 --doca-mode none --musq-group 0 --rx-burst 64 --tx-burst 64
```

Per-core report: a `Role` row (`master`, or `rx master` and `tx master` with
`--tx-masters`, and `worker`), the master's `Dealt`, `Collected`, `In flight
now`, `Self-processed` (`--dist-self`), `RX held passes` and `Collect stalls` (collect passes that found nothing
at the cursor's worker while another worker's returns were already staged
behind it: head-of-line blocking by one worker; with `seqn`, passes that
placed packets in the reorder buffer and could send none); a worker's `RX pps`
/ `TX pps` are the packets it took from and handed back to its master, its
`Cycles/pkt` the cost of the work plus the two ring hops. The master's
`Cycles/pkt` is per packet received from the NIC; a TX master's `RX pps` and
`Cycles/pkt` count the packets it collected. `--lane-epoch-us` records the
masters' (or TX masters') TX queues.

Not done (possible follow-ups): masters posting into mu_sq lanes, DOCA
round-robin steering across several masters.

### How the DOCA bring-up works

In DOCA mode EAL must not attach the PF itself. The EAL argv is built as
`-l <main>,<workers> --main-lcore ... -a 0000:00:00.0 -a auxiliary:` (the same
shape l3fwd's `pre_parse_args()` produces), then after `rte_eal_init` the PF is
probed through the DOCA DPDK bridge with `dv_flow_en=2,mu_sq_log_grp_size=<log2 group>[,devargs]`
and becomes ethdev port 0. After `rte_eth_dev_start` the steering pipeline is
installed with `l3fwd_doca_pipelines_init_groups(G, N, burst)`: G steering
groups of N queues (one group of all queues without `--musq-groups`), the
root → hash pipe → per-group STEER graph described in "Several groups". In
`--doca-mode none` the PF goes to EAL as
`-a <pci>[,mu_sq_log_grp_size=..][,devargs]`.

### Throughput

Measured ceilings of the responder and the requester, what limits each of them,
and how to reproduce the numbers are in [docs/throughput.md](docs/throughput.md).
In short: one plain RSS responder core forwards ~33 Mpps at 128 B; the port tops
out at ~114 Mpps (128 B) without MPRQ and ~157 Mpps with
`--devargs mprq_en=1,rxqs_min_mprq=1`. Link-level pause must be off on both
ports (`sudo ethtool -a enp23s0f1np1`) or a slow receiver throttles the sender.

## Traffic test with TRex

One script, `scripts/trex_run.py`, with configurable rate, frame size and
duration. Run it from danger51 (it copies itself to danger50 and runs there over
ssh) or directly on danger50 (`/homes/ofekdg/trex_scripts/trex_run.py`; the two
hosts have separate home directories). It starts the TRex server if needed, sends
one burst of `pps x duration` packets and prints TRex's tx/rx/loss as JSON.

```sh
# on danger51: start the DUT, then fire traffic
scripts/run.sh --lcores 2,4,6,8 --doca-mode esp_rr --work fixed:3000 --stats-interval 5 --stats-csv run.csv
scripts/trex_run.py --mode esp --pps 1e6 --duration 10 --frame-size 92
scripts/trex_run.py --mode udp --flows 16 --pps 2e6 --duration 10 --frame-size 1518
scripts/trex_run.py --server-only      # leave the TRex server up for repeated runs
scripts/trex_run.py --stop-server
```

`--mode esp` (IPv4/ESP, incrementing sequence number) matches `--doca-mode esp_rr`;
`--mode udp` (IPv4/UDP, `--flows` source ports) matches `--doca-mode meta_rr` and
plain RSS (`none`). `--frame-size` is bytes on the wire including the FCS (64
minimum; 92 and 1518 are the sizes used in the sweeps). The responder swaps MACs,
so the packets return to TRex's source MAC.

## Telemetry while running

```sh
sudo /homes/ofekdg/prom-dpdk/usertools/dpdk-telemetry.py -f musq
--> /eal/lcore/usage          # busy/total cycles per worker (same accounting as the CPU busy % row)
--> /mlx5/musq/txq_status     # per-lane mu_sq ring state and mu_* diag counters
--> /mlx5/txq/lane_wq         # per-lane WQ status recorded by each lane's own core
--> /ethdev/xstats,0
```

`/mlx5/txq/lane_wq` comes from the prom-dpdk mlx5 PMD (struct `mlx5_txq_lane_tel`,
compile switch `MLX5_MU_LANE_TELEMETRY`, default on). Each TX queue's own core
records, inside its `rte_eth_tx_burst`, the free WQEBBs it found at each
resource check (sum, minimum, log2 histogram, latest), the WQEBBs a post needs
(`room_need`: one lane stride plus the lap pad under mu_sq, 40 for 4 lanes),
checks below that, and calls and packets refused. It writes only its own
cache-aligned record from values the burst already computed, so there are no
cross-core loads on the hot path, and the command reads only those records, not
the lanes' ring indices; a single CPU-bound responder core measured no
difference (33.37 vs 33.30 Mpps at 128 B). Unlike `/mlx5/musq/txq_status`, which
reads each lane's live ring state from the telemetry thread, it is safe to poll
during benchmarks. `scripts/musq_lane_wq.py` prints it as per-interval rates:

```sh
sudo scripts/musq_lane_wq.py -i 1          # Ctrl-C to stop; --raw for one JSON snapshot
```

```
 port/q grp     calls/s refused%  pkts refused/s  free avg free min  need no-room% no-slot%  last free/used
  0/0     4     474,898   14.95%       2,155,892        74        0    40    0.00%   14.95%      36/16348
```

A lane whose `no-slot%` matches its `refused%` is refusing because its share of
the shared ring is full.

The per-epoch lane log (`--lane-epoch-us`) is written lane by lane when the
responder exits. Two scripts read it:

```sh
scripts/lane_epoch_report.py /tmp/lane_epoch.csv --active-only   # summary: RX balance, CPU, TX, skew, rings
scripts/lane_epoch_table.py /tmp/lane_epoch.csv                  # /tmp/lane_epoch_table.csv, one row per epoch
scripts/lane_epoch_table.py /tmp/lane_epoch.csv --long           # /tmp/lane_epoch_long.csv, one row per lane and epoch
```

With `--musq-groups` the CSV header carries `groups=` and `grp_size=` and every
row ends with the lane's `group` column (the lane's TX queue / group size).

The table is sorted by time. The wide form (default) has one row per epoch with
the lanes side by side: packets received, sent and dropped in the epoch, CPU busy
%, free and used WQEBBs, slots ahead of the slowest lane, `cq_free` and `cq_done`,
then group totals and the skew. The long form keeps the original columns, sorted
by `t_us` then lane, and adds the per-epoch packet counts and busy %. Both keep
only the span with traffic unless `--all` is given; `-o` sets the output file.

The CQ columns are the lane's own view of the TX completion queue, read through
`rte_pmd_mlx5_txq_cq_status()` (a PMD without it leaves them at zero; the log's
first line then has `cq=0`):

| column | meaning |
|---|---|
| `cq_ci` | CQ index of the lane's oldest completion request it has not consumed |
| `cq_pi` | CQ index of the next completion request the lane will make. This is the lane's counter, not the NIC's write position |
| `cq_free` | free entries of the lane's share of the CQ: `cqe_s / cq_step - (cq_pi - cq_ci) / cq_step` (both in the log's first line; a mu_sq group shares one CQ and a lane owns one entry in `cq_step`) |
| `cq_done` | of the requests made and not consumed, how many the NIC has already completed. The NIC's position is `cq_ci + cq_done * cq_step` |

`cq_done` is the only column that looks at the NIC's side. A lane makes one
request per `mu_sq_comp_n` slots (8 at one packet per WQE) and may not post
beyond what its CQ share covers, so `cq_free` falls with the free WQEBBs and
reaches zero when the lane sees its ring as full. A large `cq_done` then means
the NIC has finished and the lane has not read it yet; a small one means the NIC
has not finished. Reading it costs up to about 10 reads of the lane's own CQEs
per lane per epoch, so prefer epochs of 100 us or more when CPU cost is measured.

### Doorbell rate

The prom-dpdk mlx5 PMD counts every write to the doorbell register (the rings a
single queue makes for its sibling lanes included). The responder shows it per
core in every stats interval as `TX doorbells/s` and `TX pkts/doorbell` (WQEs
per doorbell at one packet per WQE), the per-epoch log has the cumulative count
in `doorbells` (`scripts/lane_epoch_table.py` gives the count per epoch as
`db<lane>`, `scripts/lane_epoch_report.py` the rate and slots per doorbell per
lane), and `sudo scripts/musq_lane_wq.py` prints `db/s` and `pkts/db`. The rows
are missing in a build that cannot see the PMD's counters (no-DOCA builds, the
stock PMD).

By default the PMD rings once per send pass: once per `rte_eth_tx_burst` call
when the ring has room, and once per 8 packets when a full ring makes a call
take several passes. `--db-wqes N` replaces that with one doorbell every N WQEs
of a lane, so the rate can be lowered (64, 128, ...) or raised (8, 16) at any
load:

```sh
scripts/run.sh --lcores 2,4 --doca-mode esp_rr --pkts-per-wqe 1 --txd 8192 --rxd 4096 --db-wqes 64
```

WQEs a lane has posted beyond the last doorbell are not sent until the next
one, and the other lanes wait for them too. A lane that posts nothing for
`--tx-drain-us` (100 µs) therefore sends the PMD a burst of no packets, which
makes it ring for what is held back; under continuous traffic that never fires.

## Output

Every `--stats-interval` and once at exit:

- Responder summary: RX and TX pps and wire Gbps (bytes + 4 B FCS per packet) for
  the window and as a run average, the rate offered at the port
  (`rx_phy_packets`), packets the NIC dropped before any RX queue
  (`rx_prio0_buf_discard_packets`, the NIC failing to DMA into host memory), RX
  missed (RX queue out of buffers) and received-but-not-forwarded packets. The run
  average counts from the start of measurement, so it includes any time before
  traffic arrived; use `--warmup` or read the window rate. CSV rows use scope
  `summary`.
- Port table from `rte_eth_stats`: RX/TX packets (total, +window, pps), bytes and
  bandwidth, RX errors/missed/nombuf, TX errors.
- Non-zero xstats deltas (per `--xstats`).
- Per-core table: RX/TX pps and totals, TX drops, TX doorbells/s and packets per
  doorbell (prom-dpdk PMD, see "Doorbell rate"), non-IPv4 packets, CPU busy/idle %
  (cycles of loop iterations that handled packets vs. empty polls, including the
  mock work), cycles/pkt, work µs/pkt, average RX burst size, full RX bursts.

## Requester role (phase 2): the built-in traffic generator

`--role requester` turns the same binary into a korba-style paced request
generator with RTT measurement, replacing TRex. It runs on danger50 against the
responder on danger51. Each requester lcore paces its own request stream, stamps
every request with a unique id and a service time (`request_header`, korba's
18-byte header: magic, id, `processing_time_us`), and polls its own RX queue for
responses. The responder must run with `--work header` so it spins for the time
carried in each request and flips the magic to "response".

Build on danger50 (its home directory is separate; this syncs the tree and builds
the no-DOCA variant against the system DPDK in `/opt/mellanox/dpdk`):

```sh
scripts/remote_build.sh danger50 /opt/mellanox/dpdk dpdk/pmds-26.1
```

Run:

```sh
# danger51: responder, honouring the per-request service time
scripts/run.sh --lcores 2,4,6,8 --doca-mode esp_rr --work header --tx-mode immediate --stats-interval 5

# danger50: requester, 4 lcores x 250 kps, ESP frames, 3 us service time, 30 s
ssh danger50 /homes/ofekdg/musq-testapp/scripts/run.sh --role requester --lcores 2,4,6,8 \
    --doca-mode none --musq-group 0 --traffic esp --rps 250000 --service-time fixed:3000 \
    --frame-size 92 --warmup 2 --duration 30 --stats-interval 5 --stats-csv req.csv
```

Requester options:

| option | default | meaning |
|---|---|---|
| `--rps` | 1000000 | requests per second **per requester lcore** (korba semantics); the total is printed at start |
| `--service-time` | `fixed:0` | service time asked of the responder: `fixed:<ns>`, `bimodal:<lo>/<hi>/<pct_hi>`, `exp:<mean>` (ns spec like `--work`, sent as whole microseconds) |
| `--traffic` | `esp` | `esp`: IPv4 proto 50 with SPI + incrementing SN, then the header (for `--doca-mode esp_rr`). `udp`: IPv4/UDP with the header as payload (for `meta_rr` / plain RSS) |
| `--frame-size` | 92 | bytes on the wire incl. FCS, 64..1518 |
| `--dst-mac`, `--src-mac` | `f4:20:4d:24:44:86`, port MAC | |
| `--src-ip`, `--dst-ip` | 1.1.1.1, 2.2.2.2 | with `--rx-steer flow`/`rss` lcore *i* sends from `src-ip + i` |
| `--sport`, `--dport` | 1025-1040, 12345 | udp: port or range, cycled per packet (spreads over the DUT's RSS queues) |
| `--tracker-size` | 1048576 | outstanding-request slots per lcore (power of 2) |
| `--tx-fill` | `prefill` | `prefill`: a dedicated TX mbuf pool per lcore; the frame template and the lengths are written into every mbuf once at start-up, mbufs are taken with `rte_mempool_get_bulk` (no per-mbuf reset) and only the per-request fields are written per packet (request id, source port / ESP SN, and the destination port / service time only when they vary). `copy`: `rte_pktmbuf_alloc_bulk` + `rte_memcpy` of the whole template per packet (korba behaviour) |
| `--rx-steer` | `flow` | `flow`: each lcore uses its own source IP and an ingress rte_flow rule (IPv4 dst = that IP → its queue) returns its responses to its own queue. `rss`: per-lcore source IP, plain RSS decides the queue. `none`: one source IP (ESP responses then all land on one queue) |
| `--esp-encap` | `sw` | `--traffic esp` only. `sw`: the lcores write the ESP header and sequence number; nothing is encrypted. `hw`: the lcores send plain UDP frames and the NIC encapsulates and encrypts them (see "ESP encapsulation by the NIC") |
| `--esp-sa` | `lcore` | `--esp-encap hw`: `lcore` = one IPsec SA per lcore, i.e. its own SPI (`--esp-spi` + lcore index) and its own sequence-number counter in the NIC (needs distinct source IPs: `--rx-steer flow\|rss` or `--src-ips`). `port` = one SA and one SN counter over all lcores together, the original mode; not against a responder with `--musq-groups` > 1, whose hash would split that one SN stream over the groups |
| `--esp-spi` | `0x1000` | `--traffic esp`: SPI base; lcore i writes (sw) or gets (hw) base + i with `--esp-sa lcore`, the base alone with `--esp-sa port` |
| `--src-ips` | | explicit source IPv4 per lcore, comma separated in lcore order (as many as `--lcores`), instead of `--src-ip` + index: e.g. picked from the responder's `--hash-probe` log so that each flow lands in its own mu_sq group |
| `--no-esp-sn-check` | off | `--traffic esp`: do not check that each lcore's responses come back with increasing ESP sequence numbers (see "Several groups") |
| `--tx-file` | (off) | stream correctness test: every requester lcore sends this file once, as its own flow, one chunk per request (`[8 B chunk index][file bytes]`, ESP SN = chunk index; 184 file bytes per 256 B frame), then only receives. Requests the requester's own TX refuses are sent again with the same SN |
| `--rx-record` | (off) | with `--tx-file`: record what comes back on each lcore's queue, in arrival order, to `<prefix>.w<worker>.data` (chunk bytes), `.idx` (uint64 chunk index per arrival) and `.json`, written at exit; compare with `scripts/stream_compare.py` |

The requester needs `--doca-mode none --musq-group 0` and therefore the
`build-nodoca` binary (`run.sh` selects it automatically for `--role requester`).

How matching works: request ids are `[owner lcore : 16 bits][sequence : 48 bits]`;
the slot is the low bits of the sequence. With the default `--rx-steer flow` the
responder's address swap brings every response back to the owner's own queue
(source IP `src-ip + lcore index`, one rte_flow rule per lcore), so matching is
local. A response that still arrives on another lcore (`--rx-steer rss|none`,
or traffic that misses the rules) is looked up in the owner's tracker by the
receiving lcore, which records the RTT in its own histogram and marks the slot
done. A request counts as **lost** when its slot is reused while still
outstanding (after `--tracker-size` further requests from that lcore), as
**unmatched** when a response carries an id that is no longer outstanding
(late, duplicate or stale). Without steering, ESP responses have no L4 ports,
so RSS puts them all on one queue: that lcore then does all the matching and
caps the whole run at ~7.5 M responses/s, while cross-lcore completions
cost roughly twice a local one (numbers in [docs/throughput.md](docs/throughput.md)).

Requester output adds per-core rows (Req sent, Resp matched, Resp unmatched, Req
lost, Batches capped, Non-response RX; with `--traffic esp` also ESP SN reorders,
ESP SN max back, ESP SN gap pkts and ESP foreign SPI, see "Several groups") and
an RTT table (samples, min, mean, p50,
p90, p99, p99.9, p99.99, max in µs) merged over all lcores and per lcore,
cumulative since the measurement start (after `--warmup`). CSV rows use scope
`rtt`.

Requester throughput: one lcore sends ~74 Mpps at 128 B and ~45 Mpps at 512 B;
two or more lcores reach the PCIe Gen4 x16 limit at 512 B (~55 Mpps / 225 Gbps).
For the pure-TX recipe (stopped responder, `--dst-mac 02:00:00:00:00:01`), the
per-lcore breakdown and the matching limits see [docs/throughput.md](docs/throughput.md).

Latency note: with the responder in the default `--tx-mode accumulate`, a
response waits in the TX buffer until 32 packets are collected or the 100 µs
drain timer fires, so RTT p99 sits at ~100 µs regardless of service time. Use
`--tx-mode immediate` (or a small `--tx-drain-us`) on the responder when RTT is
the metric; use `accumulate` when reproducing l3fwd's mu_sq slot behaviour.

### ESP encapsulation by the NIC (`--esp-encap hw`)

```sh
# danger50; scripts/run.sh switches to build-esphw/ (built by scripts/remote_build.sh)
scripts/run.sh --role requester --lcores 2,4 --doca-mode none --musq-group 0 \
  --traffic esp --frame-size 256 --rps 17100000 --warmup 1 --duration 10 --esp-encap hw
```

The lcores build a plain IPv4/UDP frame (request header inside) and the NIC turns
it into ESP transport mode on the way out: it inserts the SPI, a sequence number
it counts itself, an 8 B IV, the trailer and a 16 B ICV, encrypts the payload
with AES-GCM-128 and fixes the IPv4 protocol, length and checksum. This is the
NIC's IPsec packet offload, programmed through the system DOCA Flow
([src/esp_hw.cpp](src/esp_hw.cpp)): by default (`--esp-sa lcore`) one SA per
source IP, i.e. per lcore, selected by an egress rule on the source IP, and one
ingress rule per source IP that returns the responses to their owner's queue
(DOCA Flow replaces the rte_flow rule of the software mode). `--esp-sa port` is
the original mode: one SA for the whole port, applied to every IPv4 packet any
lcore sends.

- **Sequence numbers.** With `--esp-sa lcore` every lcore's packets carry their
  own SPI (`--esp-spi` + lcore index) and are numbered 0, 1, 2, .. by their own
  NIC counter in the order they leave: one lcore is one SA is one flow, as in
  the software mode. With `--esp-sa port` one counter runs over all lcores
  together, so an lcore's packets carry the numbers its bursts happened to get,
  not a run of its own; never use it against a responder with several mu_sq
  groups (see "Several groups"). The lcores keep their own source IPs with
  `--rx-steer flow|rss` (or `--src-ips`); those select the SA and steer the
  responses.
- **Frame size.** The encapsulated frame grows in steps of 4 B (ESP padding), so
  the wire frame is `--frame-size` or up to 3 B shorter: 254 B for 256 (the
  lcores send 220 B incl. FCS). The startup log gives both sizes, and the first
  four responses per lcore are described in the log (size, protocol, SPI, SN).
- **Responses stay encrypted.** Nothing decrypts them, so they are counted but
  not matched ("Non-response RX"), and there is no RTT. With an `esp_rr`
  responder that is the same as in the software mode.
- **Counters.** The port and per-queue TX byte counters are the plain frames;
  `tx_phy_bytes` and "TX Gbps (wire)" are the encapsulated ones. The NIC's count
  of encapsulated packets is logged at exit.
- Not with `--tx-file` (the file test sets the SN itself). The key is a fixed
  test key.

### Stream correctness test (`--tx-file` / `--rx-record`)

Sends a file through the DUT and checks that each flow comes back complete,
once, in order and unmodified:

```bash
# danger50: a random input whose size is a multiple of the 184 B chunk (256 B frames)
mkdir -p /dev/shm/musq-file && head -c $((184 * 16000000)) /dev/urandom > /dev/shm/musq-file/in.bin
# danger51: the DUT, e.g. mu_sq with 1-packet slots
scripts/run.sh --lcores 2,4 --doca-mode esp_rr --pkts-per-wqe 1 --txd 8192 --rxd 4096
# danger50: one flow (one lcore) at 8 Mpps; the deep RX ring and 512 bursts keep the requester from missing packets
scripts/run.sh --role requester --lcores 2 --doca-mode none --musq-group 0 --traffic esp --frame-size 256 \
  --rps 8000000 --duration 4 --rxd 8192 --txd 4096 --rx-burst 512 --tx-burst 512 \
  --tx-file /dev/shm/musq-file/in.bin --rx-record /dev/shm/musq-file/run
scripts/stream_compare.py /dev/shm/musq-file/in.bin /dev/shm/musq-file/run.w0   # VERDICT: IDENTICAL / DIFFERENT
cmp /dev/shm/musq-file/in.bin /dev/shm/musq-file/run.w0.data                     # the same, byte for byte
```

`stream_compare.py` classifies every arrival (in order, reordered with its
extent, gap, duplicate, bad index, corrupted) and lists missing chunks. Any loss
anywhere shows as missing chunks, so check the requester's "RX missed" (its own
NIC) and the responder's "TX drops" before blaming the DUT. Results of the
2026-10-05 matrix (2.9 GB per flow) are in
[docs/musq-investigation.md](docs/musq-investigation.md).

Reference run (2026-09-25): 2 requester lcores x 200 kps ESP, 3 µs service, responder
esp_rr with 2 lanes in accumulate mode: 8,007,681 requests, all matched, 0 lost,
RTT p50 58 µs / p99 104 µs; responder work 3.00 µs/pkt, busy 39.5 % per core.

## Notes

- Stale run dir after a crash: `sudo rm -rf /var/run/dpdk/musq`.
- Only one process can own the PF: stop any l3fwd/korba (danger51) or TRex
  (danger50, `scripts/trex_run.py --stop-server`) first.

## Layout

```
src/app.hpp        shared types, config struct, per-worker stats
src/config.cpp     cxxopts CLI, validation, EAL argv construction
src/bringup.cpp    DOCA probe/init, mempools, port configuration
src/work.hpp       mock work sampler (fixed/bimodal/exp/header) and busy spin
src/forward.hpp    per-packet helpers shared by the loops: tx send/flush, header swap, mock work
src/responder.cpp  worker loop: rx burst -> work -> header swap -> tx
src/dist.cpp       software distributor (--masters): master loop (rx, deal, collect in order, tx; the two halves on two lcores with --tx-masters) and worker loop
src/requester.cpp  requester role: paced generation, request tracking, cross-lcore RTT completion
src/stats.cpp      port/xstats/per-core reporting, CSV, lcore usage callback
src/main.cpp       bring-up order, monitor loop, shutdown
scripts/run.sh     sudo env -i wrapper with LD_LIBRARY_PATH
scripts/trex_run.py    TRex burst generator (pps, frame size, duration, esp/udp); runs on danger50, launchable from danger51
scripts/remote_build.sh  rsync the tree to danger50 and build the no-DOCA binary there
scripts/musq_lane_wq.py  per-lane TX WQ status from the /mlx5/txq/lane_wq PMD telemetry
scripts/lane_epoch_report.py  per-lane RX per epoch and balance, lane skew, drift and timeline from a --lane-epoch-us CSV
scripts/lane_epoch_table.py   the same CSV as one table sorted by time (wide: one row per epoch; --long: per lane)
scripts/stream_compare.py     --tx-file/--rx-record stream check: order, loss, duplicates, corruption per flow
src/lane_epoch.cpp   per-epoch lane status log (lane-owned records on a common TSC grid)
docs/throughput.md       measured requester/responder throughput, limits and recipes
docs/musq-investigation.md  status of the mu_sq TX-drop / lane-skew investigation, next steps
third_party/hdr_histogram  HdrHistogram_c core (RTT percentiles)
```
