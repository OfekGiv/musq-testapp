#!/usr/bin/env bash
# Sync this tree to another host and build the no-DOCA binary there against
# that host's DPDK (the requester runs on the traffic-generator host, danger50,
# whose home directory is not shared with danger51).
#
#   scripts/remote_build.sh [host] [dpdk_prefix] [pmd_subdir]
#   scripts/remote_build.sh danger50 /opt/mellanox/dpdk dpdk/pmds-26.1
set -euo pipefail
host="${1:-danger50}"
prefix="${2:-/opt/mellanox/dpdk}"
pmd_subdir="${3:-dpdk/pmds-26.1}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="/homes/ofekdg/musq-testapp"

rsync -a --delete --exclude 'build*/' --exclude '.runenv' --exclude '__pycache__' "$here/" "$host:$dest/"
ssh -o BatchMode=yes "$host" bash -s <<REMOTE
set -euo pipefail
cd "$dest"
printf 'PROM_INSTALL_DIR=%s\nPMD_SUBDIR=%s\n' "$prefix" "$pmd_subdir" > .runenv
if [[ ! -d build-nodoca ]]; then
  meson setup build-nodoca -Denable_doca=disabled -Dprom_install_dir="$prefix" -Ddpdk_pmd_subdir="$pmd_subdir" \
    -Dpkg_config_path="$prefix/lib/x86_64-linux-gnu/pkgconfig"
fi
ninja -C build-nodoca
ls -la build-nodoca/musq-testapp
# Requester --esp-encap hw: a second binary linked with the host's own DOCA Flow.
doca_pc=/opt/mellanox/doca/lib/x86_64-linux-gnu/pkgconfig
if [[ -f \$doca_pc/doca-flow.pc ]]; then
  if [[ ! -d build-esphw ]]; then
    meson setup build-esphw -Denable_doca=disabled -Desp_hw=enabled -Dprom_install_dir="$prefix" \
      -Ddpdk_pmd_subdir="$pmd_subdir" -Dpkg_config_path="$prefix/lib/x86_64-linux-gnu/pkgconfig:\$doca_pc"
  fi
  ninja -C build-esphw
  ls -la build-esphw/musq-testapp
fi
REMOTE
echo "built on $host: $dest/build-nodoca/musq-testapp (run with $dest/scripts/run.sh --role requester ...)"
