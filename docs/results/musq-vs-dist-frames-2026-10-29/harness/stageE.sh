#!/usr/bin/env bash
# Frame-size sweep at 2 worker cores (2026-10-29): mu_sq 2 lanes against the
# best distributor 1 master + 2 workers, 128 / 256 / 512 / 1024 B, no work and
# 100 / 200 ns of work per packet (saturation runs), then the strict rule at
# the no-work walls. ESP frames encapsulated by the requester's NIC, 80 Mpps
# offered (the requester sends what the line allows at the larger frames).
set -u
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
H=$R/harness
export BEST_OPTS="${BEST_OPTS:---rx-burst 64 --tx-burst 64 --dist-chunk 32 --dist-self 32}"
echo "== saturation $(date +%T)"
for f in 128 256 512 1024; do
  SAT_OUT=$R/sat python3 $H/sat.py --modes musq,bestw --cores 2 --works fixed:0,fixed:100,fixed:200 --frame $f --offer 80
done
echo "== strict, no work $(date +%T)"
SAT_CSV=$R/sat/sat.csv SWEEP_OUT=$R/strict python3 $H/strict_points.py \
  musq:2:fixed:0:128 bestw:2:fixed:0:128 musq:2:fixed:0:256 bestw:2:fixed:0:256 \
  musq:2:fixed:0:512 bestw:2:fixed:0:512 musq:2:fixed:0:1024 bestw:2:fixed:0:1024
echo "== done $(date +%T)"
