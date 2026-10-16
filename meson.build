project('musq-testapp', ['cpp', 'c'], version: '0.2.0',
  default_options: ['cpp_std=c++17', 'buildtype=release', 'warning_level=2', 'werror=true'])

cpp = meson.get_compiler('cpp')
fs  = import('fs')

prom_install_dir = get_option('prom_install_dir')
prom_build_dir   = get_option('prom_build_dir')
doca_sdk_root    = get_option('doca_sdk_root')
pmd_subdir       = get_option('dpdk_pmd_subdir')
enable_doca      = get_option('enable_doca')

# DPDK comes from the prom-dpdk install prefix. Configure with
#   meson setup build -Dpkg_config_path=<prom_install_dir>/lib/x86_64-linux-gnu/pkgconfig
# and refuse to silently pick up the system DPDK (/opt/mellanox/dpdk) instead.
dpdk_dep = dependency('libdpdk', version: '>=25.11')
dpdk_prefix = dpdk_dep.get_variable(pkgconfig: 'prefix')
if dpdk_prefix != prom_install_dir
  error('libdpdk.pc prefix (' + dpdk_prefix + ') != prom_install_dir (' + prom_install_dir +
        '). Re-run: meson setup <build> -Dpkg_config_path=' + prom_install_dir +
        '/lib/x86_64-linux-gnu/pkgconfig [-Dprom_install_dir=...]')
endif
dpdk_libdir = prom_install_dir / 'lib' / 'x86_64-linux-gnu'
pmd_dir     = dpdk_libdir / pmd_subdir

fmt_dep = dependency('fmt', version: '>=8')

# HdrHistogram_c core (vendored from korba's dependency overlay; no zlib log I/O).
hdr_inc = include_directories('third_party/hdr_histogram/include')
hdr_lib = static_library('hdr_histogram',
  files('third_party/hdr_histogram/src/hdr_encoding.c',
        'third_party/hdr_histogram/src/hdr_histogram.c',
        'third_party/hdr_histogram/src/hdr_histogram_log_no_op.c',
        'third_party/hdr_histogram/src/hdr_interval_recorder.c',
        'third_party/hdr_histogram/src/hdr_thread.c',
        'third_party/hdr_histogram/src/hdr_time.c',
        'third_party/hdr_histogram/src/hdr_writer_reader_phaser.c'),
  include_directories: hdr_inc,
  c_args: ['-O2', '-w'],
  dependencies: [dependency('threads')])
hdr_dep = declare_dependency(include_directories: hdr_inc, link_with: hdr_lib, link_args: ['-lm'])

cxxopts_inc = include_directories('third_party/cxxopts')
src_inc     = include_directories('src')

cpp_args = ['-O3', '-g', '-DALLOW_EXPERIMENTAL_API',
            # cxxopts 3.2 trips -Wdeprecated-copy-dtor style warnings under -Wextra on gcc 13.
            '-Wno-deprecated-declarations']

# DT_RPATH (not RUNPATH, hence --disable-new-dtags): the system DPDK in ld.so.cache
# carries the same sonames, and libdoca_dpdk_bridge.so NEEDs librte_net_mlx5.so.26
# with no RUNPATH of its own. RPATH on the executable also applies when resolving
# the dependencies of other libraries, RUNPATH does not (korba meson.build).
link_args = ['-Wl,--disable-new-dtags']
rpath_dirs = [dpdk_libdir, pmd_dir]
deps = [dpdk_dep, fmt_dep, hdr_dep]

if enable_doca.enabled()
  doca_libdir = doca_sdk_root / 'lib' / 'x86_64-linux-gnu'
  doca_inc    = doca_sdk_root / 'include'
  doca_src    = doca_sdk_root / 'src'
  sdk_a_dir   = prom_build_dir / 'doca_pipelines_sdk'
  foreach d : [doca_libdir, doca_inc, doca_src, sdk_a_dir]
    if not fs.is_dir(d)
      error('enable_doca: missing directory ' + d)
    endif
  endforeach
  pipelines_lib = cpp.find_library('doca_pipelines_meta_rr', dirs: [sdk_a_dir], static: true)
  doca_link = ['-L' + doca_libdir, '-ldoca_flow', '-ldoca_dpdk_bridge', '-ldoca_common', '-lnvhws']
  doca_dep = declare_dependency(
    compile_args: ['-DENABLE_DOCA=1', '-I' + doca_inc, '-I' + doca_src],
    link_args: doca_link,
    dependencies: [pipelines_lib])
  # The static SDK lib references rte_* symbols: keep it ahead of librte_* on the link line.
  deps = [doca_dep] + deps
  rpath_dirs += [doca_libdir]
  link_args += [# Load our patched mlx5 PMD first so the DOCA bridge reuses it by soname
                # instead of resolving librte_net_mlx5.so.26 from ld.so.cache.
                '-L' + dpdk_libdir, '-Wl,--no-as-needed', '-lrte_net_mlx5', '-Wl,--as-needed',
                '-Wl,--allow-shlib-undefined']
else
  cpp_args += ['-DENABLE_DOCA=0']
endif

# Requester --esp-encap hw: the NIC's IPsec packet offload through the system
# DOCA Flow (not the vendored pipelines SDK above; the two cannot be mixed).
if get_option('esp_hw').enabled()
  if enable_doca.enabled()
    error('esp_hw uses the system DOCA Flow: configure it with -Denable_doca=disabled')
  endif
  doca_flow_dep = dependency('doca-flow', include_type: 'system')
  deps += [doca_flow_dep,
           dependency('doca-dpdk-bridge', include_type: 'system'),
           dependency('doca-common', include_type: 'system')]
  rpath_dirs += [doca_flow_dep.get_variable(pkgconfig: 'libdir')]
  cpp_args += ['-DENABLE_ESP_HW=1']
else
  cpp_args += ['-DENABLE_ESP_HW=0']
endif

sources = files(
  'src/main.cpp',
  'src/config.cpp',
  'src/bringup.cpp',
  'src/responder.cpp',
  'src/requester.cpp',
  'src/esp_hw.cpp',
  'src/stats.cpp',
  'src/lane_epoch.cpp',
)

executable('musq-testapp', sources,
  dependencies: deps,
  include_directories: [cxxopts_inc, src_inc],
  cpp_args: cpp_args,
  link_args: link_args,
  build_rpath: ':'.join(rpath_dirs),
  install: false)
