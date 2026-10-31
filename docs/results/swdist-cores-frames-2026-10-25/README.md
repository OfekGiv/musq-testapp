# Software distributor (`--masters`) vs frame size, 2026-10-25

Highest offered rate the software distributor forwards without loss, by the
method of finding 16 (the mu_sq sweep of 2026-10-22), so the two can be read side
by side. Saturation runs that led to the tuned settings are in
[saturation/](saturation/README.md).

## Setup

- Responder, danger51: `MUSQ_BUILD=o3 scripts/run.sh --masters 2 --lcores <4,6 | …>
  --doca-mode none --musq-group 0 --txd 1024 --rxd 1024 --rx-burst 64 --tx-burst 64
  --dist-chunk 32 --stats-interval 1 --stats-csv …` (build-nodoca-o3 on
  /homes/ofekdg/dpdk-eval/prom-o3; one master, lcore 2, plus the workers; order `rr`).
- Requester, danger50: 8 lcores (2,4,…,16), `--traffic esp --esp-encap hw
  --frame-size <F> --rps <total / 8> --warmup 1 --duration 7`, as in finding 16.
- Hosts: both rebooted that morning; hugepages, mounts and pause off restored;
  governor schedutil (the lane CPUs ran at 2400 MHz when sampled, CPU 16 at 1500
  while idle), numa_balancing 1, IRQs not moved. Finding 16 had the performance
  governor, numa_balancing 0 and IRQs on odd CPUs.
- A fresh responder for every run. Steady window = the 1 s stats windows at the
  full offered rate, first and last dropped.
- Pass: TX refused ≤ 0.01 % of packets received, RX lost (offered at the port −
  delivered: RX missed and NIC discards, which is how an overloaded master shows)
  ≤ 0.01 %, offered ≥ 98 % of target. Search: 2 Mpps steps from 26 Mpps (80 % of the
  saturation ceiling), bisection to 0.5 Mpps, then 3 passing runs at the reported
  rate.
- CPU busy % as in finding 16: cycles of loop passes that handled packets / all
  TSC cycles of the lcore. For the master a pass is busy when it received or
  collected anything.

## Files

| file | content |
|---|---|
| `summary.md` | the comparison table (`harness/compare.py`) |
| `harness/cases/c<cores>_f<frame>.json` | one case each: max rate, busy %, cycles/pkt, first failing rate |
| `harness/runs.csv` | one row per measurement run |
| `harness/runs/resp_<tag>.csv` | the responder's own `--stats-csv` of each run |
| `harness/sweep.log` | the sweep log |
| `harness/` | the scripts, as run (`one_run.sh` also has `RESP_MODE=musq` for the finding-16 command) |
| `saturation/` | the three saturation batteries of the same day (raw lines, CSV, table) |

## Result (1 master + 2 workers, `rr`, bursts 64, chunk 32)

| frame (wire) | SW max Mpps (Gbps wire) | master busy, cycles/pkt | workers busy | next step fails with | mu_sq 2 cores (finding 16) | mu_sq 4 cores |
|---|---|---|---|---|---|---|
| 98 | **34.0** (26.7) | 100 %, 70 | 21-22 % | 34.5: 0.58 % RX loss | 42.0, 91 % busy | 45.0, 62 % |
| 128 (126) | **34.0** (34.3) | 100 %, 70 | 22 % | 34.5: 1.2 % RX loss | 41.5, 89 % | 44.5, 60 % |
| 256 (254) | **30.5** (62.0) | 100 %, 78 | 21-22 % | 31.0: 0.12 % RX loss | 38.0, 85 % | 39.5, 49 % |

- The master is the ceiling, at 100 % busy and 70 to 78 cycles per packet: one
  core's RX + TX cost (a run-to-completion core alone costs 73 cycles and reaches
  31.4 Mpps at 256 B in a saturation run). The worker count does not move it
  (saturation: 31.6 Mpps with 1, 3 and 7 workers). mu_sq's ceiling is the NIC's
  shared send queue, with CPU left on the lanes.
- mu_sq with two cores forwards 19 to 20 % more than the distributor with three,
  with four cores 24 % more. The distributor's workers are almost idle: about 1.6
  cores of processing headroom at these rates, against 10 to 15 % per mu_sq lane.
  With real per-packet work the comparison moves towards the distributor until
  the master saturates (not measured yet).
- Nothing was refused on TX in any run: the master's single TX queue never filled.
- 39 runs, 11 minutes. The runs' responder CSVs are in `harness/runs/`.
