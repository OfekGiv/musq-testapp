#!/usr/bin/env bash
# Repeat one responder/requester measurement <iterations> times, a fresh
# responder each time. Run on danger51 (the responder host) from the repo root:
#
#   scripts/repeat_run.sh -i <iterations> -c <lcores> -b <frame_size> -a <mpps_per_lcore>
#   scripts/repeat_run.sh -i 5 -c 2,4 -b 256 -a 17.1
#
# Each iteration:
#   1. danger51: scripts/run.sh --lcores <c> --doca-mode esp_rr --pkts-per-wqe 1
#        --txd 2048 --rxd 1024 --lane-epoch-us 50000
#        --lane-epoch-out /tmp/lane_epoch_<n>cores_<b>frame_<a>mpps_it<i>.csv
#      in the background, waiting until every worker printed "entering main loop"
#      (a signal before that kills it without writing the CSV);
#   2. danger50: scripts/run.sh --role requester --lcores <c> --doca-mode none
#        --musq-group 0 --traffic esp --frame-size <b> --rps <a * 1e6>
#        --warmup 1 --duration 3 --esp-encap hw     (blocks until it exits);
#   3. SIGINT to the danger51 responder, which stops and writes the CSV.
#
# <c> is the lcore list for both hosts (as --lcores takes it); <n> in the file
# name is the number of lcores in it. <a> is Mpps per requester lcore (--rps is
# per lcore). Logs go to <out-dir>/{resp,req}_<tag>_it<i>.log.
#
# Options: -o <out-dir> (default /tmp/musq-sweep), -R "<extra responder args>",
# -Q "<extra requester args>", -n (print the commands only).
# Refuses to start if a musq-testapp is already running on either host.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REQ_HOST="${REQ_HOST:-danger50}"
REQ_DIR="${REQ_DIR:-/homes/ofekdg/musq-testapp}"
READY_TIMEOUT_S="${READY_TIMEOUT_S:-120}"
STOP_TIMEOUT_S="${STOP_TIMEOUT_S:-60}"

iters="" lcores="" frame="" mpps="" out_dir="/tmp/musq-sweep" resp_extra="" req_extra="" dry=0
usage() { sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while getopts "i:c:b:a:o:R:Q:nh" opt; do
  case "$opt" in
    i) iters="$OPTARG" ;;
    c) lcores="$OPTARG" ;;
    b) frame="$OPTARG" ;;
    a) mpps="$OPTARG" ;;
    o) out_dir="$OPTARG" ;;
    R) resp_extra="$OPTARG" ;;
    Q) req_extra="$OPTARG" ;;
    n) dry=1 ;;
    h) usage 0 ;;
    *) usage 1 ;;
  esac
done
[[ -n "$iters" && -n "$lcores" && -n "$frame" && -n "$mpps" ]] || usage 1
[[ "$iters" =~ ^[1-9][0-9]*$ ]] || { echo "-i must be a positive integer" >&2; exit 1; }
[[ "$frame" =~ ^[0-9]+$ ]] || { echo "-b must be a frame size in bytes" >&2; exit 1; }
[[ "$mpps" =~ ^[0-9]+(\.[0-9]+)?$ ]] || { echo "-a must be Mpps per lcore, e.g. 17.1" >&2; exit 1; }
if awk -v a="$mpps" 'BEGIN { exit !(a > 1000) }'; then
  echo "-a is Mpps per requester lcore (got $mpps); --rps is derived from it" >&2; exit 1
fi
rps=$(awk -v a="$mpps" 'BEGIN { printf "%.0f", a * 1000000 }')

# Number of lcores in a list like 2,4,6 or 2-8.
ncores=0
IFS=, read -r -a parts <<<"$lcores"
for p in "${parts[@]}"; do
  if [[ "$p" =~ ^([0-9]+)-([0-9]+)$ ]]; then
    ncores=$((ncores + BASH_REMATCH[2] - BASH_REMATCH[1] + 1))
  elif [[ "$p" =~ ^[0-9]+$ ]]; then
    ncores=$((ncores + 1))
  else
    echo "-c: bad lcore list '$lcores'" >&2; exit 1
  fi
done

tag="${ncores}cores_${frame}frame_${mpps}mpps"
mkdir -p "$out_dir"

resp_cmd() {  # $1 = iteration
  echo "$here/scripts/run.sh --lcores $lcores --doca-mode esp_rr --pkts-per-wqe 1 --txd 2048 --rxd 1024" \
       "--lane-epoch-us 50000 --lane-epoch-out /tmp/lane_epoch_${tag}_it$1.csv $resp_extra"
}
req_cmd() {
  echo "cd $REQ_DIR && scripts/run.sh --role requester --lcores $lcores --doca-mode none --musq-group 0" \
       "--traffic esp --frame-size $frame --rps $rps --warmup 1 --duration 3 --esp-encap hw $req_extra"
}

if [[ "$dry" == 1 ]]; then
  for ((i = 1; i <= iters; i++)); do
    echo "it$i danger51: $(resp_cmd "$i")"
    echo "it$i $REQ_HOST: $(req_cmd)"
  done
  exit 0
fi

# Never run next to (or stop) someone else's instance.
running=$(pgrep -a -x musq-testapp || true)
running_req=$(ssh -o BatchMode=yes "$REQ_HOST" 'pgrep -a -x musq-testapp' || true)
if [[ -n "$running" || -n "$running_req" ]]; then
  echo "musq-testapp already running, not starting:" >&2
  [[ -n "$running" ]] && echo "  $(hostname): $running" >&2
  [[ -n "$running_req" ]] && echo "  $REQ_HOST: $running_req" >&2
  exit 1
fi

# Host state a reboot loses (docs/musq-investigation.md, "Host state to restore first").
for h in local "$REQ_HOST"; do
  chk='hp=$(cat /sys/devices/system/node/node*/hugepages/hugepages-1048576kB/nr_hugepages | awk "{s += \$1} END {print s}");
       p=$(sudo -n ethtool -a enp23s0f1np1 2>/dev/null | awk "/^(RX|TX):/ {print \$2}" | paste -sd/);
       echo "$(hostname) 1G-hugepages=$hp pause=$p"'
  if [[ "$h" == local ]]; then st=$(bash -c "$chk"); else st=$(ssh -o BatchMode=yes "$h" "$chk"); fi
  echo "$st"
  [[ "$st" == *"pause=off/off"* ]] || echo "warning: pause is not off ($st)" >&2
  [[ "$st" == *"1G-hugepages=0 "* ]] && echo "warning: no 1 GB hugepages ($st)" >&2
done

resp_pid=""
# The musq-testapp process under our run.sh (run.sh execs sudo, which with
# use_pty forks once more), never someone else's.
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
  for sig in INT INT KILL; do  # first SIGINT = clean stop + CSV dump, second = terminate
    if m=$(our_musq_pid); then
      sudo -n kill -s "$sig" "$m"
    else
      kill -s "$sig" "$resp_pid" 2>/dev/null  # still in sudo/run.sh: sudo relays it
    fi
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

failed=0
for ((i = 1; i <= iters; i++)); do
  rlog="$out_dir/resp_${tag}_it$i.log"
  qlog="$out_dir/req_${tag}_it$i.log"
  csv="/tmp/lane_epoch_${tag}_it$i.csv"
  echo "=== it$i/$iters $tag (rps $rps per lcore)"

  # shellcheck disable=SC2046
  $(resp_cmd "$i") </dev/null >"$rlog" 2>&1 &
  resp_pid=$!
  ready=0
  for ((t = 0; t < READY_TIMEOUT_S * 10; t++)); do
    if [[ $(grep -c 'entering main loop' "$rlog") -ge "$ncores" ]]; then ready=1; break; fi
    [[ -d /proc/$resp_pid ]] || break
    sleep 0.1
  done
  if [[ "$ready" != 1 ]]; then
    echo "it$i: responder not ready (see $rlog):" >&2
    tail -5 "$rlog" >&2
    stop_responder
    failed=$((failed + 1))
    continue
  fi

  ssh -o BatchMode=yes "$REQ_HOST" "$(req_cmd)" </dev/null >"$qlog" 2>&1
  req_rc=$?
  stop_responder

  if [[ "$req_rc" != 0 ]]; then
    echo "it$i: requester exited with $req_rc (see $qlog)" >&2
    tail -5 "$qlog" >&2
    failed=$((failed + 1))
  fi
  if [[ -s "$csv" ]]; then
    echo "it$i: $csv ($(wc -l <"$csv") lines), logs $rlog $qlog"
  else
    echo "it$i: no lane-epoch CSV at $csv (see $rlog)" >&2
    failed=$((failed + 1))
  fi
  sleep 2  # let the port settle before the next responder
done

echo "done: $iters iterations, $failed problem(s); logs in $out_dir"
[[ "$failed" == 0 ]]
