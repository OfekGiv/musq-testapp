#!/usr/bin/env bash
# Run musq-testapp under sudo with a clean environment and the runtime library
# path of the prom-dpdk shared build + the vendored DOCA bundle (same shape as
# /homes/ofekdg/prom-dpdk/readme). Override PROM_INSTALL_DIR / DOCA_SDK_ROOT /
# MUSQ_BIN via the environment.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Per-host overrides written by scripts/remote_build.sh (e.g. system DPDK on danger50).
if [[ -f "$here/../.runenv" ]]; then . "$here/../.runenv"; fi
PROM_INSTALL_DIR="${PROM_INSTALL_DIR:-/homes/ofekdg/prom-dpdk/install-o2-nometa}"
DOCA_SDK_ROOT="${DOCA_SDK_ROOT:-/homes/ofekdg/prom-dpdk/doca_pipelines_sdk}"
PMD_SUBDIR="${PMD_SUBDIR:-dpdk/pmds-26.0}"
# --doca-mode none and --role requester must run the no-DOCA build (build-nodoca):
# with the DOCA libraries loaded the mlx5 PMD delivers nothing to the RX queues
# in plain mode.
if [[ -z "${MUSQ_BIN:-}" ]]; then
  MUSQ_BIN="$here/../build/musq-testapp"
  prev=""
  for a in "$@"; do
    if [[ "$a" == "--doca-mode=none" || ( "$prev" == "--doca-mode" && "$a" == "none" ) || \
          "$a" == "--role=requester" || ( "$prev" == "--role" && "$a" == "requester" ) ]]; then
      MUSQ_BIN="$here/../build-nodoca/musq-testapp"
    fi
    prev="$a"
  done
fi
libdir="$PROM_INSTALL_DIR/lib/x86_64-linux-gnu"
ld_path="$libdir:$libdir/$PMD_SUBDIR:$DOCA_SDK_ROOT/lib/x86_64-linux-gnu"
# Extra env vars to forward into the clean environment (e.g. MUSQ_ENV="FOO=1 BAR=2").
extra_env=()
if [[ -n "${MUSQ_ENV:-}" ]]; then read -r -a extra_env <<<"$MUSQ_ENV"; fi
exec sudo -n env -i LD_LIBRARY_PATH="$ld_path" "${extra_env[@]}" "$MUSQ_BIN" "$@"
