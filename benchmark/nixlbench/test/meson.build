# SPDX-FileCopyrightText: Copyright (c) 2026 Amazon.com, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone test project for nixlbench neuron allocation tracking.
# Build: meson setup builddir && meson compile -C builddir
# Run:   meson test -C builddir

project('nixlbench-test', 'cpp',
  default_options: ['cpp_std=c++20', 'werror=true'],
  meson_version: '>= 0.64.0',
)

gtest_dep = dependency('gtest', required: true)
gtest_main_dep = dependency('gtest_main', required: true)

# Build mock NRT as a shared library named libnrt.so.1
mock_nrt = shared_library('nrt',
  'mock_nrt.cpp',
  soversion: '1',
)

# Build the test executable — links neuron.cpp directly with mock NRT.
test_neuron = executable('test_neuron_alloc',
  sources: ['test_neuron_alloc.cpp', '../src/utils/neuron.cpp'],
  dependencies: [gtest_dep, gtest_main_dep, dependency('dl', required: true)],
  include_directories: include_directories('../src/utils'),
  link_with: [mock_nrt],
)

# Set LD_LIBRARY_PATH so dlopen("libnrt.so.1") finds the mock.
test('neuron_alloc', test_neuron,
  env: ['LD_LIBRARY_PATH=' + meson.current_build_dir()],
)
