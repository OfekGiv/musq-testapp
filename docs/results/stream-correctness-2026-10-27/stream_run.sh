#!/usr/bin/env bash
# One stream correctness run: fresh responder on danger51 (RESP_ARGS), one
# requester lcore on danger50 sending /dev/shm/musq-file/in.bin as one ESP flow
# with --tx-file/--rx-record, then stream_compare.py on danger50.
#   RESP_ARGS="--lcores 2,4 ..." stream_run.sh <tag> <responder workers> [rps]
set -uo pipefail
REPO=/homes/ofekdg/musq-testapp
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/out"
REQ_HOST=danger50
tag=$1 nworkers=$2 rps=${3:-8000000}
rlog="$OUT/resp_$tag.log" qlog="$OUT/req_$tag.log" clog="$OUT/compare_$tag.txt"
mkdir -p "$OUT"; sudo -n rm -f "$OUT/resp_$tag.csv"

if pgrep -x musq-testapp >/dev/null || ssh -o BatchMode=yes $REQ_HOST 'pgrep -x musq-testapp' >/dev/null; then
  echo "musq-testapp already running on a host, not starting" >&2; exit 2
fi

resp_pid=""
our_musq_pid() {
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
    for ((t = 0; t < 600; t++)); do [[ -d /proc/$resp_pid ]] || break 2; sleep 0.1; done
  done
  wait "$resp_pid" 2>/dev/null; resp_pid=""
}
trap 'stop_responder; exit 130' INT TERM
trap 'stop_responder' EXIT

# shellcheck disable=SC2086
MUSQ_BUILD=o3 "$REPO/scripts/run.sh" $RESP_ARGS --stats-interval 1 --stats-csv "$OUT/resp_$tag.csv" \
  </dev/null >"$rlog" 2>&1 &
resp_pid=$!
for ((t = 0; t < 1200; t++)); do
  [[ $(grep -c 'entering main loop' "$rlog") -ge "$nworkers" ]] && break
  [[ -d /proc/$resp_pid ]] || { echo "$tag: responder exited (see $rlog)" >&2; tail -5 "$rlog" >&2; exit 1; }
  sleep 0.1
done

ssh -o BatchMode=yes $REQ_HOST "cd $REPO && sudo -n rm -f /dev/shm/musq-file/run_$tag.* && \
  scripts/run.sh --role requester --lcores 2 --doca-mode none --musq-group 0 --traffic esp --frame-size 256 \
  --rps $rps --duration 4 --rxd 8192 --txd 4096 --rx-burst 512 --tx-burst 512 \
  --tx-file /dev/shm/musq-file/in.bin --rx-record /dev/shm/musq-file/run_$tag" </dev/null >"$qlog" 2>&1
req_rc=$?
sleep 1.5
stop_responder

ssh -o BatchMode=yes $REQ_HOST "cd $REPO && python3 scripts/stream_compare.py /dev/shm/musq-file/in.bin /dev/shm/musq-file/run_$tag.w0; \
  echo cmp: \$(cmp /dev/shm/musq-file/in.bin /dev/shm/musq-file/run_$tag.w0.data 2>&1 && echo equal)" >"$clog" 2>&1
echo "=== $tag (requester rc=$req_rc)"
cat "$clog"
echo "--- responder (final report, per core):"
grep -E '^(TX drops|RX pkts|TX pkts) ' "$rlog" | tail -3
grep -E 'RX missed|Not forwarded' "$rlog" | tail -2
echo "--- requester (final report):"
grep -E '^(Req sent|Resp matched|Non-response RX) ' "$qlog" | tail -3
grep -E 'RX missed' "$qlog" | tail -1
