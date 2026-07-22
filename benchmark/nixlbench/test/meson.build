# SPDX-FileCopyrightText: Copyright (c) 2026 Amazon.com, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone test project for nixlbench neuron core allocation (PR #1913).
# Build: meson setup builddir && meson compile -C builddir
# Run:   meson test -C builddir

project('nixlbench-test', 'cpp',
  default_options: ['cpp_std=c++20', 'werror=true'],
  meson_version: '>= 0.64.0',
)

gtest_dep = dependency('gtest', required: true)
gtest_main_dep = dependency('gtest_main', required: true)
# neuron.cpp includes utils.h, which transitively includes <toml++/toml.hpp>.
tomlplusplus_dep = dependency('tomlplusplus', required: true)

# Minimal config.h — utils.h includes it. Host-only test: no accelerators.
configure_file(
  output: 'config.h',
  configuration: {
    'HAVE_ETCD': '0',
    'HAVE_NVSHMEM': '0',
    'HAVE_CUDA': '0',
    'HAVE_ROCM': '0',
    'HAVE_CUDA_FABRIC': '0',
  },
)

# Build mock NRT as libnrt.so.1 so neuron.cpp's dlopen("libnrt.so.1") resolves.
mock_nrt = shared_library('nrt',
  'mock_nrt.cpp',
  soversion: '1',
)

# Build the test executable — compiles neuron.cpp directly against the mock.
# Include dirs mirror the real nixlbench build:
#   '.'            -> generated config.h
#   '../src'       -> "utils.h", "runtime/runtime.h" (quoted, nixlbench-relative)
#   '../src/utils' -> "neuron.h"
#   '../../../src' -> <utils/common/nixl_time.h>, which utils.h pulls from the
#                     installed NIXL headers in a normal build; point at the
#                     in-repo source tree so the test builds standalone.
test_exe = executable('test_neuron_core_alloc',
  sources: ['test_neuron_core_alloc.cpp', '../src/utils/neuron.cpp'],
  dependencies: [gtest_dep, gtest_main_dep, tomlplusplus_dep, dependency('dl', required: true)],
  include_directories: include_directories('.', '../src', '../src/utils', '../../../src'),
  link_with: [mock_nrt],
)

ld_path = 'LD_LIBRARY_PATH=' + meson.current_build_dir()

# neuronMalloc()'s vnc selection runs per-call, so one process covers SG and MG.
test('neuron_malloc_vnc', test_exe,
  args: ['--gtest_filter=NeuronMalloc.*'],
  env: [ld_path],
)

# neuronCoreCount() caches its result in a function-local static, so it runs its
# logic only once per process. Each scenario therefore gets its own test()
# invocation (a fresh process) selecting exactly one test via --gtest_filter.
# The NEURON_RT_* environment is managed inside each test body, so only
# LD_LIBRARY_PATH needs to be passed here.
core_count_cases = [
  'SgAutoSetsNumCoresWhenUnset',
  'SgKeepsPresetSingleCoreNoWarn',
  'SgWarnsWhenNumCoresExceedsOne',
  'SgWarnsWhenVisibleCoresIsRange',
  'SgNoWarnWhenVisibleCoresSingle',
  'SgVisibleCoresRangeTakesPrecedenceOverNumCores',
  'MgDoesNotTouchNumCores',
]
foreach case : core_count_cases
  test('core_count_' + case, test_exe,
    args: ['--gtest_filter=NeuronCoreCount.' + case],
    env: [ld_path],
  )
endforeach
