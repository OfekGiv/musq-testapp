#!/usr/bin/env bash
# Release (-O3) builds of the prom-dpdk mlx5 PMD and the musq-testapp binaries
# linked against them, for lowest cycles per packet. The prom-dpdk tree is only
# read (out-of-tree meson builds); rerun this after changing the PMD source.
#
#   scripts/build_prom_o3.sh [variant...]     (default: all four)
#
# variant  PMD build                                   app build dirs (repo root)
#   o3       -O3, 32-packet slots, lane telemetry       build-o3, build-nodoca-o3
#   o3-ppw1  -O3, 1-packet slots (MLX5_MU_EMPW_PKTS=1)  build-ppw1-o3, build-nodoca-ppw1-o3
#   o3nt     o3 without lane telemetry                  build-o3nt, build-nodoca-o3nt
#   o3nt-ppw1                                           build-ppw1-o3nt, build-nodoca-ppw1-o3nt
#
# PMD build and install: $EVAL_DIR/build-prom-<variant>, $EVAL_DIR/prom-<variant>
# (EVAL_DIR defaults to /homes/ofekdg/dpdk-eval; it must not be under /tmp: EAL
# refuses to load drivers from below a world-writable directory). Select a
# build at run time with MUSQ_BUILD, e.g. MUSQ_BUILD=o3 scripts/run.sh ...
# (no-telemetry builds have no /mlx5/txq/lane_wq and no doorbell rows).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROM_SRC="${PROM_SRC:-/homes/ofekdg/prom-dpdk}"
EVAL_DIR="${EVAL_DIR:-/homes/ofekdg/dpdk-eval}"
variants=("$@")
[[ ${#variants[@]} -gt 0 ]] || variants=(o3 o3-ppw1 o3nt o3nt-ppw1)

dpdk_opts=(-Ddefault_library=shared -Dbuildtype=release -Denable_trace_fp=false
           -Dexamples= -Dtests=false -Denable_apps=test-pmd
           -Denable_drivers=bus/pci,bus/auxiliary,bus/vdev,common/mlx5,net/mlx5,mempool/ring)

for v in "${variants[@]}"; do
  cargs=()
  case "$v" in
    o3) ;;
    o3-ppw1) cargs=(-DMLX5_MU_EMPW_PKTS=1) ;;
    o3nt) cargs=(-DMLX5_MU_LANE_TELEMETRY=0) ;;
    o3nt-ppw1) cargs=(-DMLX5_MU_EMPW_PKTS=1 -DMLX5_MU_LANE_TELEMETRY=0) ;;
    *) echo "unknown variant $v" >&2; exit 1 ;;
  esac
  bdir="$EVAL_DIR/build-prom-$v" inst="$EVAL_DIR/prom-$v"
  echo "== PMD $v -> $inst"
  if [[ ! -d "$bdir" ]]; then
    meson setup "$bdir" "$PROM_SRC" "${dpdk_opts[@]}" -Dprefix="$inst" ${cargs:+-Dc_args="${cargs[*]}"} >/dev/null
  fi
  ninja -C "$bdir" >/dev/null
  meson install -C "$bdir" --quiet >/dev/null

  # app dirs: build[-nodoca][-ppw1]-<o3|o3nt>, the names scripts/run.sh derives
  tag="${v%-ppw1}" ppw=""
  [[ "$v" == *-ppw1 ]] && ppw="-ppw1"
  for doca in enabled disabled; do
    name="build"; [[ "$doca" == disabled ]] && name="build-nodoca"
    adir="$here/$name$ppw-$tag"
    echo "   app $adir"
    if [[ ! -d "$adir" ]]; then
      meson setup "$adir" "$here" -Dbuildtype=release -Denable_doca="$doca" \
        -Dpkg_config_path="$inst/lib/x86_64-linux-gnu/pkgconfig" \
        -Dprom_install_dir="$inst" -Dprom_build_dir="$bdir" >/dev/null
    fi
    ninja -C "$adir" >/dev/null
  done
done
