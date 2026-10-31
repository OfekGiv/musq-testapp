# Software distributor, UDP frames (2026-10-27)

The sweep of [../README.md](../README.md) (1 master + 2 workers, strict pass rule)
repeated with plain UDP requests instead of NIC-encapsulated ESP frames, which the
distributor does not need (only mu_sq's esp_rr steering does), and with the 64 B
frame added (the ESP-hw minimum was 98 B).

## Setup

- As in `../README.md` (same responder command, build-nodoca-o3, bursts 64, chunk 32,
  `rr` order; same search and pass rule), except the requester:
  `--traffic udp --frame-size <F> --rps <total / 8> --warmup 1 --duration 7` on 8
  lcores of danger50 (`TRAFFIC=udp` in `harness/one_run.sh`; one source IP per
  lcore, source ports cycled 1025-1040 per packet, destination port 12345).
- Both hosts rebooted that morning (09:25); hugepages, mounts and pause off were in
  place; governor schedutil, numa_balancing 1, as on 2026-10-25.
- Output of `SWEEP_OUT=udp-2026-10-27 TRAFFIC=udp harness/sweep.py --cores 3
  --frames 64,98,128,256 --start 3:26`: `cases/`, `runs/`, `runs.csv`, `sweep.log`
  in this directory; table by `harness/compare.py udp-2026-10-27`.

## Result (1 master + 2 workers, `rr`, bursts 64, chunk 32, UDP)

| frame | UDP max Mpps (Gbps wire) | master busy, cycles/pkt | workers busy | next step fails with | ESP, 2026-10-25 | mu_sq 2 cores (finding 16, ESP) |
|---|---|---|---|---|---|---|
| 64 | **35.5** (18.2) | 100 %, 67 | 22-24 % | 36.0: 2.0 % RX loss | - | - |
| 98 | **34.5** (27.0) | 100 %, 69 | 22-24 % | 35.0: 1.5 % RX loss | 34.0 | 42.0 |
| 128 | **34.0** (34.8) | 100 %, 70 | 22-23 % | 34.5: 0.7 % RX loss | 34.0 | 41.5 |
| 256 | **30.5** (62.4) | 100 %, 78 | 21-23 % | 31.0: 0.4 % RX loss | 30.5 | 38.0 |

- UDP and NIC-encapsulated ESP give the same ceiling (within one 0.5 Mpps step at
  98 B, equal at 128 and 256 B) and the same master cost: the frame contents do
  not matter to the master, which never reads them. The workers swap the UDP
  ports as well (one or two busy points more).
- 64 B adds 1 Mpps over 98 B: the master's cost per packet keeps falling with
  the frame size (67 → 69 → 70 → 78 cycles from 64 to 256 B), as a
  run-to-completion core's does.
- 47 runs, 12 minutes; nothing refused on TX in any run.
