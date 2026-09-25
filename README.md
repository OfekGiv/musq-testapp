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
| `--musq-group` | `auto` | mu_sq group size: `auto` = number of workers, `0` = off, or a power of 2 ≤ 128 |
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
| `--file-prefix` | `musq` | EAL file prefix (distinct from l3fwd's `rte`) |
| `--log-level` | 7 | EAL log level (7 = INFO) |
| `--eal-args` | | extra EAL args, e.g. `"--trace=pmd.net.mlx5.tx.lcore --trace-dir=/tmp/t"` |

Mapping from the l3fwd env knobs: `L3FWD_BUSY_WAIT_NS=3000` → `--work fixed:3000`,
`L3FWD_DOCA_GROUP` → `--doca-group`, `L3FWD_TX_DRAIN_US` → `--tx-drain-us`,
`DOCA_PIPELINES_MODE` → `--doca-mode` (the app sets the env var itself),
`--queues N` → implied by the number of `--lcores`, `--per-lcore-pool` → default.

### How the DOCA bring-up works

In DOCA mode EAL must not attach the PF itself. The EAL argv is built as
`-l <main>,<workers> --main-lcore ... -a 0000:00:00.0 -a auxiliary:` (the same
shape l3fwd's `pre_parse_args()` produces), then after `rte_eal_init` the PF is
probed through the DOCA DPDK bridge with `dv_flow_en=2,mu_sq_log_grp_size=<log2 group>[,devargs]`
and becomes ethdev port 0. After `rte_eth_dev_start` the steering pipeline is
installed for `nb_workers` queues. In `--doca-mode none` the PF goes to EAL as
`-a <pci>[,mu_sq_log_grp_size=..][,devargs]`.

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
--> /ethdev/xstats,0
```

## Output

Every `--stats-interval` and once at exit:

- Port table from `rte_eth_stats`: RX/TX packets (total, +window, pps), bytes and
  bandwidth, RX errors/missed/nombuf, TX errors.
- Non-zero xstats deltas (per `--xstats`).
- Per-core table: RX/TX pps and totals, TX drops, non-IPv4 packets, CPU busy/idle %
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
| `--src-ip`, `--dst-ip` | 1.1.1.1, 2.2.2.2 | |
| `--sport`, `--dport` | 1025-1040, 12345 | udp: port or range, cycled per packet (spreads over the DUT's RSS queues) |
| `--tracker-size` | 1048576 | outstanding-request slots per lcore (power of 2) |

The requester needs `--doca-mode none --musq-group 0` and therefore the
`build-nodoca` binary (`run.sh` selects it automatically for `--role requester`).

How matching works: request ids are `[owner lcore : 16 bits][sequence : 48 bits]`;
the slot is the low bits of the sequence. A response may arrive on any lcore
(RSS decides), so the receiving lcore looks the id up in the owner's tracker,
records the RTT in its own histogram and marks the slot done. A request counts as
**lost** when its slot is reused while still outstanding (after `--tracker-size`
further requests from that lcore), as **unmatched** when a response carries an id
that is no longer outstanding (late, duplicate or stale). ESP responses have no
L4 ports, so RSS puts them all on one queue and one lcore does all the matching;
that is expected.

Requester output adds per-core rows (Req sent, Resp matched, Resp unmatched, Req
lost, Batches capped, Non-response RX) and an RTT table (samples, min, mean, p50,
p90, p99, p99.9, p99.99, max in µs) merged over all lcores and per lcore,
cumulative since the measurement start (after `--warmup`). CSV rows use scope
`rtt`.

Latency note: with the responder in the default `--tx-mode accumulate`, a
response waits in the TX buffer until 32 packets are collected or the 100 µs
drain timer fires, so RTT p99 sits at ~100 µs regardless of service time. Use
`--tx-mode immediate` (or a small `--tx-drain-us`) on the responder when RTT is
the metric; use `accumulate` when reproducing l3fwd's mu_sq slot behaviour.

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
third_party/hdr_histogram  HdrHistogram_c core (RTT percentiles)
```
