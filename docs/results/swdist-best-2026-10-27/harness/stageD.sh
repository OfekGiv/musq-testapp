#!/usr/bin/env bash
# Stage D (2026-10-29): the best distributor counted by workers, 1 master +
# 2 / 4 / 8 workers against mu_sq 2 / 4 / 8 lanes (user: "we compare the number
# of worker cores, not including the master core"). Hosts rebooted that
# morning; hugepages, pause off, performance governor (CPUs 0, 2-18) and
# numa_balancing 0 restored first. One reference run of the 2026-10-27 point
# (best, 8 total cores, no work: 34.4 Mpps) checks the state, then the
# saturation matrix, the latency runs and the strict confirmations at every
# point where mu_sq has a strict number (finding 18).
set -u
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
H=$R/harness
export BEST_OPTS="${BEST_OPTS:---rx-burst 64 --tx-burst 64 --dist-chunk 32 --dist-self 32}"
echo "== reference point $(date +%T)"
SAT_OUT=$R/sat-workers/reference python3 $H/sat.py --modes best --cores 8 --works fixed:0 --frame 128 --offer 80
echo "== saturation, workers $(date +%T)"
SAT_OUT=$R/sat-workers python3 $H/sat.py --modes bestw --cores 2,4,8 \
  --works fixed:0,fixed:50,fixed:100,fixed:200,fixed:400,fixed:800,exp:100,exp:200 --frame 128 --offer 80
echo "== rtt $(date +%T)"
mkdir -p $R/rtt-workers
RTT_OUT=$R/rtt-workers python3 $H/rtt_runs.py --modes bestw --cores 2,4,8 --rate 16 --service 0
RTT_OUT=$R/rtt-workers python3 $H/rtt_runs.py --modes bestw --cores 4,8 --rate 30 --service 0
RTT_OUT=$R/rtt-workers python3 $H/rtt_runs.py --modes bestw --cores 2 --rate 0.5 --service 1000
RTT_OUT=$R/rtt-workers python3 $H/rtt_runs.py --modes bestw --cores 4 --rate 1.5 --service 1000
RTT_OUT=$R/rtt-workers python3 $H/rtt_runs.py --modes bestw --cores 8 --rate 4 --service 1000
echo "== strict $(date +%T)"
SAT_CSV=$R/sat-workers/sat.csv SWEEP_OUT=$R/strict-workers python3 $H/strict_points.py \
  bestw:2:fixed:0 bestw:2:fixed:100 bestw:4:fixed:0 bestw:4:fixed:50 bestw:4:fixed:100 \
  bestw:8:fixed:0 bestw:8:fixed:100 bestw:8:fixed:200
echo "== done $(date +%T)"
