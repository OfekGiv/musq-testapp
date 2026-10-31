#!/usr/bin/env bash
# Stage B of the improved-distributor test (2026-10-27): the saturation matrix
# of the best distributor (RESP_MODE=best: the master's tuned settings from
# stage A in BEST_OPTS, with --dist-self) at 2, 4 and 8 cores, 128 B, then the
# latency runs. mu_sq, plain RSS and the single master are finding 18's rows
# (same day, same host state).
#   BEST_OPTS="..." stageB.sh
set -u
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
H=$R/harness
export BEST_OPTS="${BEST_OPTS:---rx-burst 64 --tx-burst 64 --dist-chunk 32 --dist-self 32}"
echo "== BEST_OPTS=$BEST_OPTS"
echo "== saturation $(date +%T)"
SAT_OUT=$R/sat python3 $H/sat.py --modes best --cores 2,4,8 \
  --works fixed:0,fixed:50,fixed:100,fixed:200,fixed:400,fixed:800,exp:100,exp:200 --frame 128 --offer 80
echo "== rtt $(date +%T)"
mkdir -p $R/rtt
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes best --cores 2,4,8 --rate 16 --service 0
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes best --cores 4,8 --rate 30 --service 0
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes best --cores 2 --rate 0.5 --service 1000
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes best --cores 4 --rate 1.5 --service 1000
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --modes best --cores 8 --rate 4 --service 1000
echo "== done $(date +%T)"
