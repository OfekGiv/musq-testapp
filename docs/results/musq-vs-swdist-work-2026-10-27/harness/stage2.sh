#!/usr/bin/env bash
# Stage 2 of the 2026-10-27 campaign, run after the saturation matrix (sat.py):
# 1. uneven work (exp:<mean_ns>) in the same matrix, 2. latency runs at fixed
# rates, 3. strict confirmation of the crossover points of the matrix.
set -u
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
H=$R/harness
echo "== exp work $(date +%T)"
SAT_OUT=$R/sat python3 $H/sat.py --modes musq,dist,plain --cores 2,4,8 --works exp:100,exp:200 --frame 128 --offer 80
echo "== rtt $(date +%T)"
mkdir -p $R/rtt
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --cores 2,4,8 --rate 16 --service 0
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --cores 2,4,8 --rate 30 --service 0
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --cores 2,4,8 --rate 4 --service 1000
echo "== strict $(date +%T)"
SWEEP_OUT=$R/strict python3 $H/strict_points.py musq:4:fixed:50 dist:4:fixed:50 musq:8:fixed:100 dist:8:fixed:100 \
  musq:8:fixed:200 dist:8:fixed:200 plain:8:fixed:200 musq:4:fixed:100 dist:4:fixed:100 plain:4:fixed:100 \
  musq:2:fixed:100 dist:2:fixed:100
echo "== done $(date +%T)"
