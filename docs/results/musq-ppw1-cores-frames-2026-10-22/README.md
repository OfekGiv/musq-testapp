# mu_sq, 2 / 4 / 8 cores × frame size (2026-10-22)

Highest offered rate the mu_sq responder forwards before its TX queue overflows,
and CPU busy % per lane core at that rate. Write-up: finding 16 in
[../../musq-investigation.md](../../musq-investigation.md).

## Setup

- Responder, danger51: `MUSQ_BUILD=o3 scripts/run.sh --lcores <2,4 | 2,4,6,8 | 2,4,…,16>
  --doca-mode esp_rr --pkts-per-wqe 1 --txd <1024 × lanes> --rxd 1024
  --devargs mu_sq_comp_n=128 --db-wqes 128 --stats-interval 1 --stats-csv …`
  (one lane per core, mu_sq group = cores; build-ppw1-o3 on /homes/ofekdg/dpdk-eval/prom-o3-ppw1).
- Requester, danger50: 8 lcores (2,4,…,16), `--traffic esp --esp-encap hw
  --frame-size <F> --rps <total / 8> --warmup 1 --duration 7` (one NIC SA, so esp_rr
  splits evenly: lanes within 0.005 % of each other in every run).
- Hosts: performance governor (2400 MHz), pause off, hugepages, danger51
  numa_balancing 0 and IRQs on odd CPUs.
- A fresh responder for every run. Steady window = the 1 s stats windows at the
  full offered rate, first and last dropped (3 to 5 windows).
- Pass: TX refused ≤ 0.01 % of packets received, RX lost (offered at the port −
  delivered) ≤ 0.01 %, offered ≥ 98 % of target. Search: 2 Mpps steps from 80 % of
  the expected wall, bisection to 0.5 Mpps (0.25 for 1024 and 1518 B), then 3 passing
  runs at the reported rate.
- CPU busy % = cycles of loop passes that handled packets / all TSC cycles of the
  lcore (the app's counters, same as `/eal/lcore/usage`).

## Files

| file | content |
|---|---|
| `musq-ppw1-cores-frames.png` | the graph: Mpps and CPU busy % vs frame size |
| `musq-ppw1-pps-vs-frame.png` | pps vs frame size from `runs.csv`, one marker shape per core count; lines with filled markers = pps (left axis), hollow markers alone = average CPU busy % of the lane cores (right axis) (`harness/plot_pps.py`) |
| `musq-ppw1-cores-frames-summary.csv` | one row per case (18) |
| `runs.csv` | one row per measurement run (178) |
| `runs_lcores.csv` | one row per run and lane core (852) |
| `raw/resp_<tag>.csv` | the responder's own `--stats-csv` of each run: every counter per 1 s window |
| `harness/` | the scripts and the sweep log, as run |
| `lane-epoch/` | 2 cores with `--lane-epoch-us 25`: per-lane `wqe_ci`, `wqe_pi`, `wqe_free`, CQ state and counters every 25 µs (gzipped), plus each run's stats CSV. 256 B at 34 / 38 / 40 / 42 Mpps (17 / 19 / 20 / 21 per core): CI − PI median 502 / 864 / 1696 / 1920 WQEBBs, refused 0 / 0.002 / 0.59 / 6.3 %. 512 B at 28 / 29.5 / 31 Mpps: 0 / 0.67 / 6.2 % refused |
| `slides/` | the two deck charts, rendered by `harness/plot_slides.py . <lane-epoch dir> <out>`: `slide_ci_pi.png` (CI − PI over 2 ms at 256 B) and `slide_ceiling.png` |

`summary.csv` columns: `max_mpps` = highest passing offered rate (total over all
lanes); `forwarded_mpps`, `gbps_wire` (incl. FCS) and the CPU columns are means over
the 3 passing runs at it; `cpu_avg` = mean busy % over the lane cores, `cpu_max` =
busiest core, `cpu_peak_1s` = busiest core in any single 1 s window;
`cyc_per_pkt` = busy cycles per received packet, per core; `first_fail_*` = the
next step up: its rate, what failed (TX = refusals), TX refused % and RX loss %.

`runs.csv` columns: `target_mpps` (offered total), `phase` (ladder / bisect /
confirm), `pass`, `limit`, `offered_mpps` (rx_phy_packets at the port),
`rx_mpps` (delivered to the RX queues), `tx_mpps` (accepted by rte_eth_tx_burst),
`tx_refused_pct`, `rx_loss_pct`, `nic_discards`, `rx_missed`, `wire_bytes`,
`rx_gbps_wire`, `cpu_avg`, `cpu_max`, `cpu_peak_1s`, `cyc_per_pkt_avg`,
`rx_share_spread_pct` (lane imbalance), `windows`, `busy` (JSON, per lcore).

`raw/*.csv` columns: `timestamp_s, window_index, elapsed_s, window_s, is_final,
scope, id, metric, total, delta, rate`; `is_final = 1` is the since-start report at
exit. Scopes: `summary` (port rx/tx pps, offered_pps, nic_discards, Gbps), `port`
(ethdev stats), `core` (per lcore: rx/tx packets, tx_drops = refused,
cycles_used / cycles_wasted, doorbells, …), `xstat`.
