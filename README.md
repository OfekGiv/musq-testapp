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
(the DOCA libraries link shared DPDK).

## Run

`scripts/run.sh` runs the binary under `sudo env -i` with the right
`LD_LIBRARY_PATH` (same shape as the prom-dpdk readme). Env overrides:
`PROM_INSTALL_DIR`, `DOCA_SDK_ROOT`, `MUSQ_BIN`, `MUSQ_ENV` (extra vars).

```sh
# 4 mu_sq lanes, ESP sequence-number round robin, 3 us of mock work per packet
scripts/run.sh --lcores 2,4,6,8 --doca-mode esp_rr --work fixed:3000 --stats-interval 5

# plain RSS baseline, no mu_sq, no DOCA (run.sh switches to build-nodoca/)
scripts/run.sh --lcores 2,4,6,8 --doca-mode none --musq-group 0

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
| `--musq-group` | `auto` | mu_sq group size: `auto` = number of TX queues (workers × `--tx-lanes`), `0` = off, or a power of 2 ≤ 128. With one worker and `--tx-lanes 1`, a group of several lanes is driven by that worker's single TX queue (see "One core, two lanes") |
| `--tx-lanes` | 1 | responder, one worker lcore: TX queues (mu_sq lanes) that lcore posts to, dealing the packets to them in turn. A power of 2 ≤ 8 |
| `--db-wqes` | 0 | responder with mu_sq: WQEs a lane posts per doorbell. The PMD then rings the lane's doorbell every that many WQEs (mu_sq slots), wherever a burst ends, instead of once per send pass (mlx5 devarg `mu_sq_db_wqes`; at most half the slots a lane has in the ring). `0` keeps the PMD's rule. See "Doorbell rate" |
| `-l, --lcores` | required | worker lcores (`2,4,6,8` or `2-8`); one RX + one TX queue each, queue id = worker index |
| `--main-lcore` | `0` | EAL main lcore (stats only) |
| `--doca-mode` | `esp_rr` | `none` / `meta_rr` (IPv4+UDP) / `esp_rr` (IPv4+ESP, steer on ESP SN). Both pipelines drop non-matching traffic in the NIC |
| `--doca-group` | `1` | steering group: consecutive packets per queue before moving on (was `L3FWD_DOCA_GROUP`) |
| `--rx-burst` / `--tx-burst` | 32 / 32 | rx ≤ tx, tx a power of 2. Under mu_sq one `rte_eth_tx_burst` call ≤ tx-burst = one eMPW slot |
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

### How the DOCA bring-up works

In DOCA mode EAL must not attach the PF itself. The EAL argv is built as
`-l <main>,<workers> --main-lcore ... -a 0000:00:00.0 -a auxiliary:` (the same
shape l3fwd's `pre_parse_args()` produces), then after `rte_eal_init` the PF is
probed through the DOCA DPDK bridge with `dv_flow_en=2,mu_sq_log_grp_size=<log2 group>[,devargs]`
and becomes ethdev port 0. After `rte_eth_dev_start` the steering pipeline is
installed for `nb_workers` queues. In `--doca-mode none` the PF goes to EAL as
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
lost, Batches capped, Non-response RX) and an RTT table (samples, min, mean, p50,
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
it into ESP transport mode on the way out: it inserts SPI 0x1000, a sequence
number it counts itself, an 8 B IV, the trailer and a 16 B ICV, encrypts the
payload with AES-GCM-128 and fixes the IPv4 protocol, length and checksum. This
is the NIC's IPsec packet offload, programmed through the system DOCA Flow
([src/esp_hw.cpp](src/esp_hw.cpp)): one SA for the whole port, applied by an
egress rule to every IPv4 packet any lcore sends, and one ingress rule per
source IP that returns the responses to their owner's queue (DOCA Flow replaces
the rte_flow rule of the software mode).

- **Sequence numbers.** One counter in the NIC for all lcores together, starting
  at 0, in the order the packets leave. An lcore's own packets therefore carry
  the numbers its bursts happened to get, not a run of its own. (In the software
  mode every lcore counts its own.) The lcores keep their own source IPs with
  `--rx-steer flow|rss`; those only steer the responses.
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
src/responder.cpp  worker loop: rx burst -> work -> header swap -> tx
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
