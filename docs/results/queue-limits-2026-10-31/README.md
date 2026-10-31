# What limits one queue at 128 B (2026-10-31)

Why does mu_sq forward about 44 Mpps when testpmd with one queue stops at about
37 (35 at 128 B)? Offered-rate ladders on five queue layouts, a fresh DUT per run,
with the NIC port counters of both hosts read around every run. Write-up: finding
22 in [../../musq-investigation.md](../../musq-investigation.md). Workbook with the
charts: [../queue-limits-128B-2026-10-31.xlsx](../queue-limits-128B-2026-10-31.xlsx).

| Layout (128 B) | Nothing lost up to | Most forwarded | What stops it |
|---|---|---|---|
| testpmd: 1 RX queue, 1 TX queue | 34 Mpps | 35.6 | RX: excess lost on RX, TX refuses nothing, core 90 % |
| musq-testapp: 1 RX queue, 1 TX queue | 34 Mpps | 35.4 | RX: same, core 97 % |
| musq-testapp: 2 RX queues, 1 TX queue, multi-packet WQEs | 60 Mpps | 62.1 | the core (100 %), TX refuses nothing |
| musq-testapp: 2 RX queues, 1 TX queue, 1 packet per WQE | 38 Mpps | 42.6 | TX: refusals with RX complete from 42 to 48 offered |
| mu_sq: 2 cores, 2 RX queues, shared ring, 1 packet per WQE | 41 Mpps | 43.7 | TX: refusals with RX complete up to 56 offered |

Packet accounting across the link, all 89 runs: danger50 TX = danger51 RX and
danger51 TX = danger50 RX, difference 0, over 211 to 451 M packets per run. The wire
counter is within 0.003 Mpps of the driver's TX count.

## Method

- DUT danger51, lcores 20 (and 22), main lcore 18; requester danger50, 8 lcores,
  paced, 7 s of traffic. Commands: the `CASES` table in `harness/qrun.py`.
- Performance governor, numa_balancing 0, pause off, hugepages, NIC async IRQ on
  CPU 4 (no lane there). musq-testapp on prom-dpdk at -O3; testpmd stock DPDK 25.11.
- Pass: TX refused and RX lost each at most 0.01 %, offered at least 98 % of target.
  One run per rate, three around each limit. "Nothing lost up to" needs every run at
  that rate and below to pass; 40 Mpps on the 1-WQE plain queue passed once in three.
- musq-testapp rates: the app's 1 s windows, first and last of the full-rate windows
  dropped. testpmd rates: its per-second Rx-pps/Tx-pps, offered from the NIC counters.

## What differs between the layouts besides the queues

Plain-queue runs: UDP, RSS, SEND WQEs at 1 packet per WQE, one core, testpmd-like
loop flags (burst 64, immediate, no header swap). mu_sq: ESP with esp_rr, one-packet
eMPW slots, a CQE and a doorbell every 128 slots, the sweep's default loop flags, two
cores. So the 2.6 % between the last two rows is not a measured advantage of either.

## Files

| file | content |
|---|---|
| `runs.csv` | one row per run: rates, loss, CPU, port counters of both hosts |
| `raw/` | each run's DUT log, requester log and (musq-testapp) per-second stats CSV |
| `forwarded-vs-offered.png` | the workbook's main chart drawn with matplotlib, for viewing without Excel |
| `harness/` | `qrun.py` (runner), `build_xlsx.py` (workbook), `plot_preview.py` (PNG) |
