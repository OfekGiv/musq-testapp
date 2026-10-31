#!/usr/bin/env bash
# Stage 1 of the split-pipeline test (2026-10-27): the saturation matrix of
# the split distributor (RX master + TX master + cores - 2 workers) and the
# single-master distributor (rebuilt today, so measured again) at 4 and 8
# cores, 128 B, then the latency runs. mu_sq and plain RSS are not run again:
# their binaries' code did not change, the numbers are those of
# ../musq-vs-swdist-work-2026-10-27 (same day, same host state).
set -u
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
H=$R/harness
echo "== saturation $(date +%T)"
SAT_OUT=$R/sat python3 $H/sat.py --modes split,dist --cores 4,8 \
  --works fixed:0,fixed:50,fixed:100,fixed:200,fixed:400,fixed:800,exp:100,exp:200 --frame 128 --offer 80
echo "== rtt $(date +%T)"
mkdir -p $R/rtt
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes split,dist --cores 4,8 --rate 16 --service 0
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes split,dist --cores 4,8 --rate 30 --service 0
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes split --cores 4 --rate 1.5 --service 1000
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes split --cores 8 --rate 4 --service 1000
echo "== done $(date +%T)"
