| frame (wire) | SW 1 master + 2: max Mpps, Gbps | master busy %, cycles/pkt | workers busy % | first fail | mu_sq 2 cores (finding 16) | mu_sq 4 cores | mu_sq 8 cores |
|---|---|---|---|---|---|---|---|
| 64 (64) | **35.5 Mpps**, 18.17 Gbps (2 workers) | 100.0 %, 67 | 22-24 % | 36 (RX, RX loss 2.026 %) | - | - | - |
| 98 (98) | **34.5 Mpps**, 27.04 Gbps (2 workers) | 100.0 %, 69 | 22-24 % | 35 (RX, RX loss 1.546 %) | 42 Mpps, busy 91 % | 45 Mpps, busy 62 % | 42 Mpps, busy 47 % |
| 128 (128) | **34 Mpps**, 34.8 Gbps (2 workers) | 100.0 %, 70 | 22-23 % | 34.5 (RX, RX loss 0.689 %) | 41.5 Mpps, busy 89 % | 44.5 Mpps, busy 60 % | 41.5 Mpps, busy 45 % |
| 256 (256) | **30.5 Mpps**, 62.44 Gbps (2 workers) | 99.9 %, 78 | 21-23 % | 31 (RX, RX loss 0.442 %) | 38 Mpps, busy 85 % | 39.5 Mpps, busy 49 % | 35.5 Mpps, busy 37 % |
