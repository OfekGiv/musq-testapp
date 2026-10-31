#!/usr/bin/env bash
# One measurement (copied from swdist-cores-frames-2026-10-25/harness and
# extended with a plain mode, a work axis and the latency traffic).
# RESP_MODE=dist (default): software distributor on danger51, one master
# (lcore 2) + (cores - 1) workers (lcores 4, 6, ...), --doca-mode none, no
# mu_sq, rxd 1024, txd 1024, bursts 64, chunk 32 (DIST_OPTS), MUSQ_BUILD=o3
# (build-nodoca-o3). RESP_MODE=musq: the finding-16 command (1 pkt/WQE, one
# lane per core, txd 1024 x lanes, comp_n 128, db-wqes 128 (MUSQ_OPTS; set it
# empty for the PMD defaults), build-ppw1-o3).
# RESP_MODE=plain: <cores> run-to-completion lcores with one RSS queue each
# (--doca-mode none, no mu_sq, no ordering; PLAIN_OPTS = bursts 64 as the
# distributor), the no-ordering reference.
# WORK=<spec> adds --work <spec> to the responder (fixed:<ns>, exp:<mean_ns>,
# header); RESP_EXTRA = further responder args (e.g. --tx-mode immediate).
# Requester on danger50 (8 lcores, total rate split evenly), as in finding 16:
# TRAFFIC=esp (default) NIC-encapsulated ESP, udp plain UDP, esp_sw ESP written
# by the lcores (readable header: the responder with WORK=header answers it and
# the requester matches responses and measures RTT); REQ_EXTRA = further
# requester args (e.g. --service-time fixed:1000).
#   one_run.sh <cores> <frame> <total_mpps> <tag> [req_duration_s]
# Writes $OUT/{resp_<tag>.csv,resp_<tag>.log,req_<tag>.log}. Exit 2 = someone
# else's musq-testapp is running (nothing started), 1 = run failed.
set -uo pipefail
REPO=/homes/ofekdg/musq-testapp
OUT="${OUT:-/tmp/musq-sweep/runs}"
REQ_HOST="${REQ_HOST:-danger50}"
REQ_LCORES="${REQ_LCORES:-2,4,6,8,10,12,14,16}"
READY_TIMEOUT_S="${READY_TIMEOUT_S:-120}"
STOP_TIMEOUT_S="${STOP_TIMEOUT_S:-60}"
cores=$1 frame=$2 mpps=$3 tag=$4 dur=${5:-7}
RESP_MODE="${RESP_MODE:-dist}"
work_args="${WORK:+--work $WORK}"
lcores=$(seq -s, 2 2 $((2 * cores)))        # musq: one lane per core
wlcores=$(seq -s, 4 2 $((2 * cores)))       # dist: workers; the master is lcore 2
txd=$((1024 * cores))
nreq=$(tr ',' '\n' <<<"$REQ_LCORES" | wc -l)
rps=$(awk -v a="$mpps" -v n="$nreq" 'BEGIN { printf "%.0f", a * 1e6 / n }')
rcsv="$OUT/resp_$tag.csv" rlog="$OUT/resp_$tag.log" qlog="$OUT/req_$tag.log"
mkdir -p "$OUT"; rm -f "$rcsv"

running=$(pgrep -a -x musq-testapp || true)
running_req=$(ssh -o BatchMode=yes "$REQ_HOST" 'pgrep -a -x musq-testapp' || true)
if [[ -n "$running" || -n "$running_req" ]]; then
  echo "musq-testapp already running, not starting: ${running:+danger51: $running} ${running_req:+$REQ_HOST: $running_req}" >&2
  exit 2
fi

resp_pid=""
our_musq_pid() {  # the musq-testapp under our run.sh (sudo forks once more)
  local p q
  for p in $(pgrep -x musq-testapp); do
    q=$p
    while [[ "$q" -gt 1 ]]; do
      [[ "$q" == "$resp_pid" ]] && { echo "$p"; return 0; }
      q=$(awk '/^PPid:/ {print $2}' "/proc/$q/status" 2>/dev/null) || break
      [[ -n "$q" ]] || break
    done
  done
  return 1
}
stop_responder() {
  [[ -n "$resp_pid" && -d /proc/$resp_pid ]] || { resp_pid=""; return 0; }
  local m t sig
  for sig in INT INT KILL; do
    if m=$(our_musq_pid); then sudo -n kill -s "$sig" "$m"; else kill -s "$sig" "$resp_pid" 2>/dev/null; fi
    for ((t = 0; t < STOP_TIMEOUT_S * 10; t++)); do
      [[ -d /proc/$resp_pid ]] || break 2
      sleep 0.1
    done
    echo "responder did not exit within ${STOP_TIMEOUT_S}s after SIG$sig" >&2
  done
  wait "$resp_pid" 2>/dev/null
  resp_pid=""
}
trap 'stop_responder; exit 130' INT TERM
trap 'stop_responder' EXIT

t0=$(date +%s.%N)
if [[ "$RESP_MODE" == dist ]]; then
  [[ "$cores" -ge 2 ]] || { echo "dist needs >= 2 cores (1 master + workers)" >&2; exit 1; }
  # DIST_OPTS: the master's best settings from the saturation runs (bursts of
  # 64, whole bursts per worker); override to measure others.
  MUSQ_BUILD=o3 "$REPO/scripts/run.sh" --masters 2 --lcores "$wlcores" --doca-mode none --musq-group 0 \
    --txd 1024 --rxd 1024 ${DIST_OPTS---rx-burst 64 --tx-burst 64 --dist-chunk 32} $work_args \
    --stats-interval 1 --stats-csv "$rcsv" ${RESP_EXTRA:-} </dev/null >"$rlog" 2>&1 &
elif [[ "$RESP_MODE" == plain ]]; then
  MUSQ_BUILD=o3 "$REPO/scripts/run.sh" --lcores "$lcores" --doca-mode none --musq-group 0 \
    --txd 1024 --rxd 1024 ${PLAIN_OPTS---rx-burst 64 --tx-burst 64} $work_args \
    --stats-interval 1 --stats-csv "$rcsv" ${RESP_EXTRA:-} </dev/null >"$rlog" 2>&1 &
elif [[ "$RESP_MODE" == musq ]]; then
  MUSQ_BUILD=o3 "$REPO/scripts/run.sh" --lcores "$lcores" --doca-mode esp_rr --pkts-per-wqe 1 \
    --txd "$txd" --rxd 1024 ${MUSQ_OPTS---devargs mu_sq_comp_n=128 --db-wqes 128} $work_args \
    --stats-interval 1 --stats-csv "$rcsv" ${RESP_EXTRA:-} </dev/null >"$rlog" 2>&1 &
else
  echo "RESP_MODE must be dist, plain or musq" >&2; exit 1
fi
resp_pid=$!
ready=0
for ((t = 0; t < READY_TIMEOUT_S * 10; t++)); do
  if [[ $(grep -c 'entering main loop' "$rlog") -ge "$cores" ]]; then ready=1; break; fi
  [[ -d /proc/$resp_pid ]] || break
  sleep 0.1
done
t1=$(date +%s.%N)
if [[ "$ready" != 1 ]]; then
  echo "$tag: responder not ready (see $rlog)" >&2; tail -5 "$rlog" >&2
  exit 1
fi

# TRAFFIC=esp (default): ESP frames encapsulated by the requester's NIC, as in
# finding 16 (needs mu_sq's esp_rr steering); udp: plain UDP requests (frame
# >= 64 B), which the distributor and plain RSS take as well; esp_sw: ESP
# written by the lcores, header readable (RTT runs, responder WORK=header).
case "${TRAFFIC:-esp}" in
  udp) traffic_args="--traffic udp" ;;
  esp_sw) traffic_args="--traffic esp --esp-encap sw" ;;
  esp) traffic_args="--traffic esp --esp-encap hw" ;;
  *) echo "TRAFFIC must be esp, udp or esp_sw" >&2; exit 1 ;;
esac
ssh -o BatchMode=yes "$REQ_HOST" "cd $REPO && scripts/run.sh --role requester --lcores $REQ_LCORES \
  --doca-mode none --musq-group 0 $traffic_args --frame-size $frame --rps $rps \
  --warmup 1 --duration $dur ${REQ_EXTRA:-}" </dev/null >"$qlog" 2>&1
req_rc=$?
sleep 1.5   # one more responder stats window after the traffic stops
stop_responder
t2=$(date +%s.%N)
awk -v a="$t0" -v b="$t1" -v c="$t2" -v tag="$tag" 'BEGIN { printf "%s: ready in %.1fs, total %.1fs\n", tag, b - a, c - a }'
if [[ "$req_rc" != 0 ]]; then echo "$tag: requester exited with $req_rc (see $qlog)" >&2; tail -5 "$qlog" >&2; exit 1; fi
[[ -s "$rcsv" ]] || { echo "$tag: no stats CSV" >&2; exit 1; }
exit 0
