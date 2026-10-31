#!/usr/bin/env bash
# Stage 3 (after the stage-2 latency runs): mu_sq with the PMD's own doorbell
# rule instead of --db-wqes 128 (MUSQ_OPTS empty), the 1 us service rows below
# capacity for 2 and 4 cores, and the distributor with --dist-chunk 1.
set -u
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
H=$R/harness
echo "== musq default doorbell $(date +%T)"
export MUSQ_OPTS=""
RTT_OUT=$R/rtt-musq-dbdefault python3 $H/rtt_runs.py --modes musq --cores 2,4,8 --rate 16 --service 0
RTT_OUT=$R/rtt-musq-dbdefault python3 $H/rtt_runs.py --modes musq --cores 2,4,8 --rate 30 --service 0
RTT_OUT=$R/rtt-musq-dbdefault python3 $H/rtt_runs.py --modes musq --cores 2 --rate 0.5 --service 1000
RTT_OUT=$R/rtt-musq-dbdefault python3 $H/rtt_runs.py --modes musq --cores 4 --rate 1.5 --service 1000
RTT_OUT=$R/rtt-musq-dbdefault python3 $H/rtt_runs.py --modes musq --cores 8 --rate 4 --service 1000
unset MUSQ_OPTS
echo "== 1 us service below capacity $(date +%T)"
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --cores 2 --rate 0.5 --service 1000
RTT_OUT=$R/rtt python3 $H/rtt_runs.py --cores 4 --rate 1.5 --service 1000
echo "== distributor chunk 1 $(date +%T)"
export DIST_OPTS="--rx-burst 64 --tx-burst 64 --dist-chunk 1"
RTT_OUT=$R/rtt-dist-chunk1 python3 $H/rtt_runs.py --modes dist --cores 8 --rate 16 --service 0
RTT_OUT=$R/rtt-dist-chunk1 python3 $H/rtt_runs.py --modes dist --cores 8 --rate 4 --service 1000
echo "== done $(date +%T)"
