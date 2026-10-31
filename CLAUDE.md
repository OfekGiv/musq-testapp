# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A C++17 DPDK test application with two roles in one binary: a **responder** that sends every received packet back (after optional mock per-packet work) and a **requester** (`--role requester`) that is a paced traffic generator with RTT measurement. It exists to evaluate **mu_sq** ("PROM"), a feature of the user's fork of the mlx5 PMD in `/homes/ofekdg/prom-dpdk` (a separate repo, outside this one; do not commit or revert changes there unasked), and to compare it with a software alternative (`--masters`, the distributor) and plain RSS. [README.md](README.md) is the reference for every option, the build variants and measured numbers; [docs/musq-investigation.md](docs/musq-investigation.md) holds the numbered findings and the host-state table; [docs/throughput.md](docs/throughput.md) the generator/responder ceilings. Read those instead of re-deriving.

## Hosts and safety

- Real hardware, no simulator. **danger51** = responder/DUT (ConnectX-9 `0000:17:00.1`), **danger50** = requester. Their home directories are **not shared**: code reaches danger50 only through `scripts/remote_build.sh danger50 ...`, and the requester is run there with `ssh danger50 .../scripts/run.sh --role requester ...`.
- The user runs their own responder/requester by hand. Before starting anything run `pgrep -a -x musq-testapp` on **both** hosts, stop only a PID you started, never send traffic into their responder, and run tests/measurements only when the request asks for them.
- Host setup is lost at every reboot (hugepages, 1 GB mounts, pause frames off, governor, `numa_balancing`, IRQ affinity). Restore with `scripts/setup_hugepages_pause.sh danger51 danger50` and the "Host state to restore first" table in `docs/musq-investigation.md`; stale pause frames or NUMA balancing make loss figures misleading. Only one process can own the PF; a crashed run may need `sudo rm -rf /var/run/dpdk/musq`.

## Build

meson + ninja, `werror=true` and `warning_level=2` (a warning breaks the build). **There is no unit-test suite and no linter**; verification is a traffic run, `--dry-run` (prints the resolved config and EAL argv without touching the NIC), and `scripts/stream_compare.py` for flow integrity.

```sh
P=/homes/ofekdg/prom-dpdk/install-o2-nometa
meson setup build        -Dpkg_config_path=$P/lib/x86_64-linux-gnu/pkgconfig   # DOCA modes (esp_rr, meta_rr)
meson setup build-nodoca -Denable_doca=disabled -Dpkg_config_path=$P/lib/x86_64-linux-gnu/pkgconfig
ninja -C build-nodoca
```

The build directories are separate **binary variants**, and `scripts/run.sh` picks one from its own argv (it never builds): `--doca-mode none` or `--role requester` → `build-nodoca` (the DOCA libraries loaded in-process make the mlx5 PMD deliver nothing to RX queues in plain mode, so a DOCA build refuses `none`); `--pkts-per-wqe 1` → `-ppw1` (a second prom-dpdk built with `-DMLX5_MU_EMPW_PKTS=1`, because the mu_sq slot size is a compile-time PMD constant); `--esp-encap hw` → `build-esphw`; `MUSQ_BUILD=o3|o3nt` → `-o3`/`-o3nt` release builds from `scripts/build_prom_o3.sh`. So after editing `src/`, rebuild **every variant you are about to run** (`ninja -C build-nodoca-ppw1`, ...) or you measure a stale binary. After changing the PMD itself, rerun `scripts/build_prom_o3.sh` for the `-o3` variants (README "Build").

## Run

```sh
scripts/run.sh --lcores 2,4 --doca-mode esp_rr --pkts-per-wqe 1 --txd 8192 --rxd 4096   # responder, mu_sq 2 lanes
scripts/run.sh --lcores 2,4 --doca-mode none --musq-group 0                              # plain RSS
scripts/run.sh --masters 2 --lcores 4,6 --doca-mode none --musq-group 0                  # software distributor
build/musq-testapp --lcores 2,4 --dry-run                                                # config check only
```

`run.sh` execs the binary under `sudo -n env -i` with an explicit `LD_LIBRARY_PATH`; the binary also carries a DT_RPATH to the prom-dpdk libs so the system DPDK in `/opt/mellanox/dpdk` is never picked up (`ldd build/musq-testapp | grep rte_`). The line `entering main loop on lcore N` is the ready signal harnesses wait for, so keep `--log-level 7`. Ctrl-C stops cleanly (a second signal kills); a final report is always printed.

## Architecture

Control flow is in `src/main.cpp`: `parse_cli` → `validate_config` → `build_eal_argv` → `rte_eal_init` → DOCA probe (or plain EAL attach) → `setup_workers` → `create_pools` → `configure_port` → (DOCA pipeline install) → role init → `rte_eal_remote_launch(worker_entry)` per lcore → `monitor_loop` on the main lcore (warmup, periodic report, duration). `worker_entry` dispatches on `WorkerCtx::role` / `dist_role` to `responder_loop`, `requester_loop`, or `dist_master_loop` / `dist_tx_master_loop` / `dist_worker_loop`.

- **`AppConfig` (src/app.hpp) is the single source of truth.** Options are registered in `parse_cli`; cross-option constraints (which modes combine, power-of-2 sizes, queue counts) live in `validate_config`. The derived queue/lcore counts (`nb_rx_queues()`, `nb_tx_queues()`, `musq_group_eff()`, ...) change meaning under `--masters`, where the *masters* own the NIC queues and workers own none; use these helpers rather than `lcores.size()`.
- **DOCA bring-up is unusual** (README "How the DOCA bring-up works"): in DOCA mode EAL is started with `-a 0000:00:00.0 -a auxiliary:` so it does not attach the PF; `doca_probe` probes it afterwards through the DOCA DPDK bridge with the mu_sq devargs, and `doca_init` installs the steering pipeline after `rte_eth_dev_start`. In `--doca-mode none` the PF is a normal EAL `-a`. `ENABLE_DOCA`/`ENABLE_ESP_HW` are compile-time macros set by meson; `esp_hw` (system DOCA Flow) and the vendored pipelines SDK cannot be linked together.
- **Per-lcore state is single-writer.** `WorkerStats`/`WorkerCtx` are cache-line aligned, written only by the owning lcore and read relaxed by the main lcore; lane-epoch records (`lane_epoch.cpp`) follow the same rule on a common TSC grid. Keep it that way: cross-core loads on the data path are exactly what these experiments measure.
- **mu_sq groups (`--musq-groups G`).** RX queue i and TX queue i belong to the same lcore; group g = queues g·N..g·N+N−1 on both sides. The PMD derives G from `txqs_n / N` (devarg `mu_sq_log_grp_size` only), the DOCA pipeline gets (G, N) from the app (`l3fwd_doca_pipelines_init_groups`), so the three must agree: `validate_config` enforces one queue per lane. The group decision is made in the NIC (hash pipe) ahead of the SN round robin; never pick a group in software. The SN round robin keeps a flow's order only with one SA per group, hence the requester's `--esp-sa lcore` default; `--esp-sa port` must not be sent to several groups.
- **Responder data path** is `responder.cpp` + `forward.hpp` (`process_burst`: mock work from `work.hpp`, header swap, `tx_send`/flush with the accumulate/immediate TX modes). Under mu_sq one `rte_eth_tx_burst` ≤ `--tx-burst` is one slot; `--tx-lanes` and the single-queue-drives-the-group mode are described in README "One core, two lanes".
- **Software distributor** (`dist.cpp/.hpp`): a master deals *tokens* (frame address + length, never the mbuf) to workers over `rte_ring`s and collects them back in order; mbuf pointers stay in master-local FIFOs so workers never touch mbuf cache lines. Order is implicit (`rr`, collect cursor mirrors deal cursor) or via sequence numbers + reorder buffer (`seqn`). No shared counters for flow control; `Ring retries` must stay 0. `--tx-masters` splits a master over two lcores; `--dist-self` makes the master process packets too. The master loop is templated (`ORDER`, `RX`, `TX`, `SELF`): **keep run-time variants as template parameters, not branches, in the per-packet loop**, and after any edit to it A/B the 8-core 0-work saturation point against the previous binary (a lambda and a 4 KB stack array once cost 6–12 cycles/packet on a ~69-cycle master loop).
- **Requester** (`requester.cpp`): request ids are `[owner lcore:16][sequence:48]`; with the default `--rx-steer flow` each lcore has its own source IP and an ingress rte_flow rule so responses return to the owning queue and matching is local. A response landing on another lcore is completed cross-core via the owner's tracker.
- **Reporting** (`stats.cpp`): per-interval responder summary, port table, xstats deltas, per-core table, `--stats-csv`; the final report is produced after workers stop. `--lane-epoch-us` dumps per-lane CSVs at exit, read by `scripts/lane_epoch_report.py` / `lane_epoch_table.py`.

## Gotchas

- `rte_eal_init` frees some of the argv strings it is given: never free them afterwards (double free at exit).
- EAL `--log-level 6` hides the INFO ready line; the app default is 7.
- danger51's `/` has only about 4 GB free: a `--lane-epoch` CSV of millions of records is 0.7–1 GB, so keep them small and delete them.
- The mu_sq and PMD-telemetry paths (`lane_pmd_init`, `/mlx5/txq/lane_wq`, doorbell counters) only exist with the prom-dpdk PMD; builds against the stock PMD report them as missing, which is expected.

## Test results

- All test results are maintained in an Excel sheet. Existing workbooks live in `docs/results/*.xlsx`.
- Each campaign also gets a dated directory `docs/results/<topic>-<date>/` with a `README.md` (setup, method, table), the raw run data, and a `harness/` of the scripts that produced it (`one_run.sh`, `sweep.py`, `build_xlsx.py`, ...). Summarise the conclusion as a numbered finding in `docs/musq-investigation.md`.
- There is no LibreOffice on the hosts, so formulas in a workbook cannot be recalculated there: write measured numbers as plain values and build charts as native Excel charts on those cells (see `docs/results/queue-limits-2026-10-31/harness/build_xlsx.py`, openpyxl).
- Comparison rules for mu_sq vs the distributor: count the distributor's **worker** cores (the master excluded) against mu_sq lanes, compare only the best distributor (`--dist-self`), and keep "strict" (loss-free) results complete.

## Graphs

- Every line in a graph must have a unique symbol (marker), so lines stay distinguishable without relying on color. This applies to matplotlib PNGs (`marker=`) and to native Excel charts (openpyxl `Marker(symbol=...)`) alike.
