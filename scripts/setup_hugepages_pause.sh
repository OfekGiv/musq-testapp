#!/usr/bin/env bash
# Set the hugepages and turn Ethernet pause frames off, on this host or on
# other hosts over ssh. None of it survives a reboot: run it after every boot,
# before starting DPDK.
#
#   scripts/setup_hugepages_pause.sh                     # this host
#   scripts/setup_hugepages_pause.sh danger51 danger50   # each host over ssh
#
# Per NUMA node: HP_1G pages of 1 GB and HP_2M pages of 2 MB, mounted at
# /mnt/huge-1G and /mnt/huge; global pause (RX and TX) off on IFACE.
# Needs passwordless sudo on each host. Prints the resulting state per host
# and exits non-zero if any host did not get all of it.
#
# Env: HP_1G (default 4), HP_2M (2048), IFACE (enp23s0f1np1).
set -uo pipefail
HP_1G="${HP_1G:-4}"
HP_2M="${HP_2M:-2048}"
IFACE="${IFACE:-enp23s0f1np1}"

setup_local() {
  local ok=1 node got mnt size
  for node in /sys/devices/system/node/node[0-9]*; do
    echo "$HP_1G" | sudo -n tee "$node/hugepages/hugepages-1048576kB/nr_hugepages" >/dev/null || ok=0
    echo "$HP_2M" | sudo -n tee "$node/hugepages/hugepages-2048kB/nr_hugepages" >/dev/null || ok=0
  done
  for mnt in /mnt/huge-1G:1G /mnt/huge:2M; do
    size=${mnt#*:} mnt=${mnt%%:*}
    sudo -n mkdir -p "$mnt" || ok=0
    mountpoint -q "$mnt" || sudo -n mount -t hugetlbfs -o "pagesize=$size" nodev "$mnt" || ok=0
  done
  # ethtool -A fails when nothing changes on some versions; the read-back below decides.
  sudo -n ethtool -A "$IFACE" rx off tx off >/dev/null 2>&1

  local hp1 hp2 pause
  hp1=$(cat /sys/devices/system/node/node*/hugepages/hugepages-1048576kB/nr_hugepages | paste -sd/)
  hp2=$(cat /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages | paste -sd/)
  pause=$(sudo -n ethtool -a "$IFACE" 2>/dev/null | awk '/^(RX|TX):/ {print $2}' | paste -sd/)
  for got in ${hp1//\// }; do [[ "$got" -ge "$HP_1G" ]] || ok=0; done
  for got in ${hp2//\// }; do [[ "$got" -ge "$HP_2M" ]] || ok=0; done
  mountpoint -q /mnt/huge-1G && mountpoint -q /mnt/huge || ok=0
  [[ "$pause" == "off/off" ]] || ok=0
  printf '%s: 1G pages/node %s, 2M pages/node %s, mounts %s, pause rx/tx %s -> %s\n' \
    "$(hostname)" "$hp1" "$hp2" "$(grep hugetlbfs /proc/mounts | awk '{print $2}' | paste -sd,)" \
    "${pause:-unknown}" "$([[ $ok == 1 ]] && echo OK || echo FAILED)"
  [[ $ok == 1 ]]
}

if [[ $# -eq 0 ]]; then
  setup_local
  exit
fi

rc=0
for h in "$@"; do
  if [[ "$h" == "$(hostname)" || "$h" == localhost ]]; then
    setup_local || rc=1
  else
    # The other host need not have this repo: send the script over stdin.
    ssh -o BatchMode=yes "$h" "HP_1G=$HP_1G HP_2M=$HP_2M IFACE=$IFACE bash -s" <"${BASH_SOURCE[0]}" || rc=1
  fi
done
exit $rc
