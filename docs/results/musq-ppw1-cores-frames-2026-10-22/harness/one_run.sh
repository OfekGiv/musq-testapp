#!/usr/bin/env bash
# One measurement: fresh mu_sq responder on danger51 (1 pkt/WQE, one lane per
# core, rxd 1024, txd 1024 x lanes, comp_n 128, db-wqes 128, MUSQ_BUILD=o3),
# NIC-ESP requester on danger50 (8 lcores, total rate split evenly).
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
lcores=$(seq -s, 2 2 $((2 * cores)))
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
MUSQ_BUILD=o3 "$REPO/scripts/run.sh" --lcores "$lcores" --doca-mode esp_rr --pkts-per-wqe 1 \
  --txd "$txd" --rxd 1024 --devargs mu_sq_comp_n=128 --db-wqes 128 \
  --stats-interval 1 --stats-csv "$rcsv" ${RESP_EXTRA:-} </dev/null >"$rlog" 2>&1 &
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

ssh -o BatchMode=yes "$REQ_HOST" "cd $REPO && scripts/run.sh --role requester --lcores $REQ_LCORES \
  --doca-mode none --musq-group 0 --traffic esp --esp-encap hw --frame-size $frame --rps $rps \
  --warmup 1 --duration $dur ${REQ_EXTRA:-}" </dev/null >"$qlog" 2>&1
req_rc=$?
sleep 1.5   # one more responder stats window after the traffic stops
stop_responder
t2=$(date +%s.%N)
awk -v a="$t0" -v b="$t1" -v c="$t2" -v tag="$tag" 'BEGIN { printf "%s: ready in %.1fs, total %.1fs\n", tag, b - a, c - a }'
if [[ "$req_rc" != 0 ]]; then echo "$tag: requester exited with $req_rc (see $qlog)" >&2; tail -5 "$qlog" >&2; exit 1; fi
[[ -s "$rcsv" ]] || { echo "$tag: no stats CSV" >&2; exit 1; }
exit 0
