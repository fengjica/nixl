/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Amazon.com, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */

// Unit tests for the Neuron SG-mode core-allocation logic in neuron.cpp:
//   - neuronCoreCount(): in SG mode, auto-sets NEURON_RT_NUM_CORES=1 when no
//     core env is set, and warns only when the user scoped more than one core
//     (NEURON_RT_VISIBLE_CORES taking precedence over NEURON_RT_NUM_CORES).
//   - neuronMalloc(): selects vnc=0 in SG mode, vnc=devid otherwise.
//
// neuron.cpp reaches the Neuron runtime via dlopen("libnrt.so.1"); the test
// build links a mock libnrt.so.1 (mock_nrt.cpp) found through LD_LIBRARY_PATH.
//
// neuronCoreCount() caches its result in a function-local static, so its logic
// runs only once per process. Each NeuronCoreCount.* case is therefore run in
// its own process (see meson.build) and calls neuronCoreCount() exactly once.

#include "neuron.h"
#include "utils.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>

// neuron.cpp references only this xferBenchConfig static; define it here so the
// test links without pulling in utils.cpp and the rest of nixlbench.
std::string xferBenchConfig::mode = "";

// Mock hook: records the vnc index passed to the last nrt_tensor_allocate().
extern "C" int nixlbench_test_last_vnc;

namespace {

// Redirects std::cerr into a buffer for the lifetime of the object so tests can
// inspect the diagnostics neuronCoreCount() emits.
class CerrCapture {
  public:
    CerrCapture() : prev_(std::cerr.rdbuf(buffer_.rdbuf())) {}
    ~CerrCapture() {
        std::cerr.rdbuf(prev_);
    }
    std::string
    str() const {
        return buffer_.str();
    }

  private:
    std::ostringstream buffer_;
    std::streambuf *prev_;
};

void
clearCoreEnv() {
    unsetenv("NEURON_RT_NUM_CORES");
    unsetenv("NEURON_RT_VISIBLE_CORES");
}

constexpr const char *kScopeWarning = "scopes more cores";

} // namespace

// ---------------------------------------------------------------------------
// neuronCoreCount() — SG-mode core scoping (one call per process; see above).
// ---------------------------------------------------------------------------

TEST(NeuronCoreCount, SgAutoSetsNumCoresWhenUnset) {
    clearCoreEnv();
    xferBenchConfig::mode = XFERBENCH_MODE_SG;

    CerrCapture cerr;
    // Mock nrt_get_visible_vnc_count() reports 2 cores.
    ASSERT_EQ(neuronCoreCount(), 2);

    const char *num_cores = getenv("NEURON_RT_NUM_CORES");
    ASSERT_NE(num_cores, nullptr);
    EXPECT_STREQ(num_cores, "1");
    EXPECT_NE(cerr.str().find("NEURON_RT_NUM_CORES=1"), std::string::npos);
    EXPECT_EQ(cerr.str().find(kScopeWarning), std::string::npos);
}

TEST(NeuronCoreCount, SgKeepsPresetSingleCoreNoWarn) {
    clearCoreEnv();
    // A correct single-core setup: do not warn, do not override.
    setenv("NEURON_RT_NUM_CORES", "1", 1);
    xferBenchConfig::mode = XFERBENCH_MODE_SG;

    CerrCapture cerr;
    ASSERT_EQ(neuronCoreCount(), 2);

    EXPECT_STREQ(getenv("NEURON_RT_NUM_CORES"), "1");
    EXPECT_EQ(cerr.str().find(kScopeWarning), std::string::npos);
}

TEST(NeuronCoreCount, SgWarnsWhenNumCoresExceedsOne) {
    clearCoreEnv();
    setenv("NEURON_RT_NUM_CORES", "4", 1);
    xferBenchConfig::mode = XFERBENCH_MODE_SG;

    CerrCapture cerr;
    ASSERT_EQ(neuronCoreCount(), 2);

    const std::string out = cerr.str();
    EXPECT_NE(out.find(kScopeWarning), std::string::npos);
    EXPECT_NE(out.find("NEURON_RT_NUM_CORES=4"), std::string::npos);
    // NEURON_RT_NUM_CORES must not be overridden when the user set it.
    EXPECT_STREQ(getenv("NEURON_RT_NUM_CORES"), "4");
}

TEST(NeuronCoreCount, SgWarnsWhenVisibleCoresIsRange) {
    clearCoreEnv();
    setenv("NEURON_RT_VISIBLE_CORES", "0-1", 1);
    xferBenchConfig::mode = XFERBENCH_MODE_SG;

    CerrCapture cerr;
    ASSERT_EQ(neuronCoreCount(), 2);

    const std::string out = cerr.str();
    EXPECT_NE(out.find(kScopeWarning), std::string::npos);
    EXPECT_NE(out.find("NEURON_RT_VISIBLE_CORES=0-1"), std::string::npos);
}

TEST(NeuronCoreCount, SgNoWarnWhenVisibleCoresSingle) {
    clearCoreEnv();
    // A single visible core is fine — SG mode only uses the first one.
    setenv("NEURON_RT_VISIBLE_CORES", "3", 1);
    xferBenchConfig::mode = XFERBENCH_MODE_SG;

    CerrCapture cerr;
    ASSERT_EQ(neuronCoreCount(), 2);

    EXPECT_EQ(cerr.str().find(kScopeWarning), std::string::npos);
}

TEST(NeuronCoreCount, SgVisibleCoresRangeTakesPrecedenceOverNumCores) {
    clearCoreEnv();
    // Both set: NEURON_RT_VISIBLE_CORES wins, and its range scopes >1 core.
    setenv("NEURON_RT_VISIBLE_CORES", "0-1", 1);
    setenv("NEURON_RT_NUM_CORES", "1", 1);
    xferBenchConfig::mode = XFERBENCH_MODE_SG;

    CerrCapture cerr;
    ASSERT_EQ(neuronCoreCount(), 2);

    const std::string out = cerr.str();
    EXPECT_NE(out.find(kScopeWarning), std::string::npos);
    // The warning should name the authoritative variable, not NUM_CORES.
    EXPECT_NE(out.find("NEURON_RT_VISIBLE_CORES=0-1"), std::string::npos);
}

TEST(NeuronCoreCount, MgDoesNotTouchNumCores) {
    clearCoreEnv();
    xferBenchConfig::mode = XFERBENCH_MODE_MG;

    CerrCapture cerr;
    ASSERT_EQ(neuronCoreCount(), 2);

    // MG mode leaves core scoping to the default (claims all cores).
    EXPECT_EQ(getenv("NEURON_RT_NUM_CORES"), nullptr);
    EXPECT_EQ(cerr.str().find(kScopeWarning), std::string::npos);
}

// ---------------------------------------------------------------------------
// neuronMalloc() — vnc selection (runs per-call, so one process covers both).
// ---------------------------------------------------------------------------

TEST(NeuronMalloc, SgUsesVncZero) {
    xferBenchConfig::mode = XFERBENCH_MODE_SG;

    void *addr = nullptr;
    nixlbench_test_last_vnc = -1;
    // devid is deliberately non-zero to prove SG mode ignores it.
    ASSERT_EQ(neuronMalloc(&addr, 4096, /*devid=*/1), 0);
    ASSERT_NE(addr, nullptr);
    EXPECT_EQ(nixlbench_test_last_vnc, 0);

    neuronFree(addr);
}

TEST(NeuronMalloc, MgUsesDevid) {
    xferBenchConfig::mode = XFERBENCH_MODE_MG;

    void *addr = nullptr;
    nixlbench_test_last_vnc = -1;
    ASSERT_EQ(neuronMalloc(&addr, 4096, /*devid=*/1), 0);
    ASSERT_NE(addr, nullptr);
    EXPECT_EQ(nixlbench_test_last_vnc, 1);

    neuronFree(addr);
}
