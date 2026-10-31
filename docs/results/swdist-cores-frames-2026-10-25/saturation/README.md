# Saturation runs of the software distributor, 2026-10-25

Offered rate far above the ceiling (8 requester lcores on danger50, ESP frames with NIC
encapsulation, 64 Mpps for 98 and 256 B, 48 for 512 B, 18 reached for 1518 B), 5 s runs,
busiest 1 s window. "fwd" = packets transmitted by the responder; the rest is dropped by the
NIC (RX missed / NIC discards). Master/plain core: cycles per packet received and CPU busy %.
danger51, lane CPUs at 2400 MHz (schedutil), numa_balancing 1, pause off. Raw lines in
`battery{1,2,3}.log`, same data in `saturation.csv`.

| battery / design | config | frame B | offered | fwd Mpps | master or plain core: cyc/pkt, busy, avg burst | workers: busy, cyc/pkt |
|---|---|---|---|---|---|---|
| battery1: mbuf pointers through the rings (first design) | plain, 1 core | 256 | 64.00 | **31.24** | 75, 99.0%, 24.6 |   |
| battery1: mbuf pointers through the rings (first design) | plain, 2 cores (RSS) | 256 | 64.00 | **46.38** | 79/116, 95.8%/84.9%, 23.5/12.3 |   |
| battery1: mbuf pointers through the rings (first design) | 1 master + 1 worker | 256 | 63.99 | **19.15** | 125, 100.0%, 32.0 | 40-40 % 49-49 |
| battery1: mbuf pointers through the rings (first design) | 1 + 3 | 256 | 64.00 | **17.11** | 139, 100.0%, 32.0 | 25-29 % 103-119 |
| battery1: mbuf pointers through the rings (first design) | 1 + 7 | 256 | 64.00 | **14.37** | 166, 100.0%, 32.0 | 11-16 % 127-184 |
| battery1: mbuf pointers through the rings (first design) | 1 + 3, seqn | 256 | 64.00 | **19.39** | 123, 100.0%, 32.0 | 28-35 % 104-128 |
| battery1: mbuf pointers through the rings (first design) | 1 + 3, 1 pkt/WQE | 256 | 64.00 | **15.68** | 152, 100.0%, 32.0 | 24-28 % 110-126 |
| battery1: mbuf pointers through the rings (first design) | 2 masters + 6 workers | 256 | 64.00 | **33.48** | 143/142, 100.0%/100.0%, 32.0/31.9 | 23-27 % 100-116 |
| battery1: mbuf pointers through the rings (first design) | plain, 1 core | 98 | 64.00 | **34.94** | 68, 99.5%, 28.6 |   |
| battery1: mbuf pointers through the rings (first design) | 1 master + 1 worker | 98 | 64.00 | **18.78** | 127, 100.0%, 32.0 | 39-39 % 49-49 |
| battery1: mbuf pointers through the rings (first design) | 1 + 3 | 98 | 64.00 | **17.27** | 138, 100.0%, 32.0 | 26-29 % 106-119 |
| battery1: mbuf pointers through the rings (first design) | 1 + 7 | 98 | 64.00 | **14.47** | 165, 100.0%, 32.0 | 11-15 % 129-171 |
| battery1: mbuf pointers through the rings (first design) | plain, 1 core | 1518 | 17.98 | **13.76** | 85, 49.3%, 19.8 |   |
| battery1: mbuf pointers through the rings (first design) | 1 master + 1 worker | 1518 | 18.00 | **13.63** | 155, 88.7%, 23.6 | 31-31 % 54-54 |
| battery1: mbuf pointers through the rings (first design) | 1 + 3 | 1518 | 17.98 | **13.66** | 167, 95.8%, 26.3 | 21-22 % 110-117 |
| battery1: mbuf pointers through the rings (first design) | 1 + 7 | 1518 | 18.00 | **13.70** | 174, 99.9%, 31.5 | 10-13 % 125-154 |
| battery2: mbuf pointers (first design) | 1 + 3 | 256 | 64.00 | **17.08** | 140, 100.0%, 32.0 | 25-28 % 103-119 |
| battery2: mbuf pointers (first design) | 1 + 3, TX inline off | 256 | 64.00 | **17.15** | 139, 100.0%, 32.0 | 24-29 % 102-119 |
| battery2: mbuf pointers (first design) | 1 + 3, bursts 64 | 256 | 64.00 | **20.10** | 119, 100.0%, 64.0 | 22-29 % 79-102 |
| battery2: mbuf pointers (first design) | 1 + 3, 2 RX queues | 256 | 64.00 | **16.79** | 142, 100.0%, 32.0 | 24-27 % 103-113 |
| battery2: mbuf pointers (first design) | 1 + 3, chunk 8 | 256 | 64.00 | **21.04** | 113, 100.0%, 32.0 | 30-34 % 101-114 |
| battery2: mbuf pointers (first design) | 1 + 3, chunk 32 | 256 | 64.00 | **23.07** | 103, 100.0%, 32.0 | 16-16 % 49-50 |
| battery2: mbuf pointers (first design) | 1 + 3, immediate TX | 256 | 64.00 | **16.91** | 141, 100.0%, 32.0 | 24-28 % 103-119 |
| battery2: mbuf pointers (first design) | 1 + 3, inline off + bursts 64 + chunk 32 | 256 | 64.00 | **25.11** | 95, 100.0%, 64.0 | 25-33 % 71-95 |
| battery2: mbuf pointers (first design) | plain, 1 core, TX inline off | 256 | 64.00 | **31.83** | 74, 99.3%, 25.8 |   |
| battery2: mbuf pointers (first design) | 1 + 3, o3 build | 256 | 64.00 | **16.92** | 141, 100.0%, 32.0 | 24-28 % 102-118 |
| battery3: tokens (frame address + length) | 1 + 1 | 256 | 64.00 | **23.56** | 101, 100.0%, 32.0 | 28-28 % 28-28 |
| battery3: tokens (frame address + length) | 1 + 3 | 256 | 64.00 | **21.02** | 113, 100.0%, 32.0 | 16-20 % 54-66 |
| battery3: tokens (frame address + length) | 1 + 7 | 256 | 64.00 | **17.59** | 136, 100.0%, 32.0 | 10-13 % 96-124 |
| battery3: tokens (frame address + length) | 1 + 3, seqn | 256 | 64.00 | **22.88** | 104, 100.0%, 32.0 | 19-22 % 58-67 |
| battery3: tokens (frame address + length) | 1 + 3, chunk 32 | 256 | 64.00 | **29.58** | 80, 100.0%, 32.0 | 18-20 % 44-48 |
| battery3: tokens (frame address + length) | 1 + 3, bursts 64 | 256 | 63.97 | **25.62** | 93, 100.0%, 64.0 | 12-14 % 33-38 |
| battery3: tokens (frame address + length) | 1 + 3, bursts 64 + chunk 32 | 256 | 63.78 | **31.60** | 75, 100.0%, 58.0 | 16-17 % 35-39 |
| battery3: tokens (frame address + length) | 1 + 3, seqn + bursts 64 + chunk 32 | 256 | 64.00 | **25.43** | 94, 100.0%, 64.0 | 12-15 % 33-43 |
| battery3: tokens (frame address + length) | 1 + 7, bursts 64 + chunk 32 | 256 | 63.72 | **31.62** | 75, 100.0%, 56.8 | 6-7 % 33-39 |
| battery3: tokens (frame address + length) | 1 + 1, bursts 64 + chunk 32 | 256 | 64.00 | **30.63** | 78, 100.0%, 49.8 | 33-33 % 25-25 |
| battery3: tokens (frame address + length) | 2 masters + 6 workers, bursts 64 + chunk 32 | 256 | 64.00 | **45.11** | 84/136, 99.7%/96.7%, 43.9/17.3 | 13-16 % 38-57 |
| battery3: tokens (frame address + length) | plain, 1 core, bursts 64 | 256 | 64.00 | **31.41** | 73, 96.8%, 21.5 |   |
| battery3: tokens (frame address + length) | 1 + 3, bursts 64 + chunk 32 | 98 | 64.00 | **34.26** | 69, 100.0%, 64.0 | 17-19 % 35-39 |
| battery3: tokens (frame address + length) | 1 + 3, bursts 64 + chunk 32 | 512 | 48.00 | **25.17** | 94, 99.0%, 28.7 | 14-16 % 40-44 |

## Reading

- First design (mbuf pointers through the rings, per-packet round robin): a master forwards 17 to 19 Mpps,
  125 to 166 cycles per packet, more workers cost more (ring operations per pass grow with the worker count).
- Knobs on that design: TX inlining, the o3 build, immediate TX and two RX queues change nothing; 64-packet
  bursts (119 cycles) and chunks of 8 / 32 packets per worker (113 / 103) help; together 95 cycles, 25 Mpps.
- Token design (the frame address and length cross the rings, the mbufs stay with the master): 24 to 30 cycles
  less in every configuration; with bursts of 64 and chunks of 32 the master costs 75 cycles per packet and
  forwards 31.6 Mpps at 256 B (34.3 at 98 B, 25.2 at 512 B) with 1, 3 or 7 workers alike, the same as one
  run-to-completion core alone (31.4 Mpps, 73 cycles). The master is the ceiling; a second master adds
  capacity (45 Mpps with two, RSS split 8 flows unevenly between them).
- `seqn` costs 94 cycles with the tuned settings (its collector polls every worker every pass).
