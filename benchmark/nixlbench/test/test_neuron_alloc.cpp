/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Amazon.com, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */

// Unit tests for neuron.cpp allocation tracking (range lookup, bounds checks).
// Requires mock_nrt.so to be preloaded so dlopen("libnrt.so.1") resolves.

#include "neuron.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

class NeuronAllocTest : public ::testing::Test {
  protected:
    void TearDown() override {
        for (void *p : allocs_) {
            neuronFree(p);
        }
    }
    void *allocAndTrack(size_t size) {
        void *addr = nullptr;
        int rc = neuronMalloc(&addr, size);
        EXPECT_EQ(rc, 0);
        EXPECT_NE(addr, nullptr);
        if (addr) allocs_.push_back(addr);
        return addr;
    }
    std::vector<void *> allocs_;
};

TEST_F(NeuronAllocTest, BasicAllocFree) {
    void *addr = nullptr;
    ASSERT_EQ(neuronMalloc(&addr, 4096), 0);
    ASSERT_NE(addr, nullptr);
    ASSERT_EQ(neuronFree(addr), 0);
}

TEST_F(NeuronAllocTest, FreeNull) {
    ASSERT_EQ(neuronFree(nullptr), 0);
}

TEST_F(NeuronAllocTest, DoubleFree) {
    void *addr = nullptr;
    ASSERT_EQ(neuronMalloc(&addr, 4096), 0);
    ASSERT_EQ(neuronFree(addr), 0);
    ASSERT_EQ(neuronFree(addr), -1);
}

TEST_F(NeuronAllocTest, MemcpyAtBase) {
    void *addr = allocAndTrack(1024);
    std::vector<uint8_t> src(1024, 0xAB);
    ASSERT_EQ(neuronMemcpy(addr, src.data(), 1024, neuronMemcpyHostToDevice), 0);

    std::vector<uint8_t> dst(1024, 0);
    ASSERT_EQ(neuronMemcpy(dst.data(), addr, 1024, neuronMemcpyDeviceToHost), 0);
    ASSERT_EQ(src, dst);
}

TEST_F(NeuronAllocTest, MemcpyAtOffset) {
    constexpr size_t alloc_size = 8192;
    constexpr size_t offset = 4096;
    constexpr size_t copy_len = 2048;

    void *base = allocAndTrack(alloc_size);
    void *mid = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(base) + offset);

    std::vector<uint8_t> src(copy_len, 0xCD);
    ASSERT_EQ(neuronMemcpy(mid, src.data(), copy_len, neuronMemcpyHostToDevice), 0);

    std::vector<uint8_t> dst(copy_len, 0);
    ASSERT_EQ(neuronMemcpy(dst.data(), mid, copy_len, neuronMemcpyDeviceToHost), 0);
    ASSERT_EQ(src, dst);
}

TEST_F(NeuronAllocTest, MemcpyOutOfBounds) {
    void *addr = allocAndTrack(1024);
    std::vector<uint8_t> buf(2048, 0);
    // Trying to copy more than the allocation size should fail.
    ASSERT_EQ(neuronMemcpy(addr, buf.data(), 2048, neuronMemcpyHostToDevice), -1);
}

TEST_F(NeuronAllocTest, MemcpyOffsetOutOfBounds) {
    constexpr size_t alloc_size = 4096;
    void *base = allocAndTrack(alloc_size);
    // Point to offset 4000, try to copy 200 bytes -> exceeds allocation.
    void *near_end = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(base) + 4000);
    std::vector<uint8_t> buf(200, 0);
    ASSERT_EQ(neuronMemcpy(near_end, buf.data(), 200, neuronMemcpyHostToDevice), -1);
}

TEST_F(NeuronAllocTest, MemcpyUnknownAddress) {
    void *bogus = reinterpret_cast<void *>(static_cast<uintptr_t>(0xDEADBEEF));
    std::vector<uint8_t> buf(64, 0);
    ASSERT_EQ(neuronMemcpy(bogus, buf.data(), 64, neuronMemcpyHostToDevice), -1);
}

TEST_F(NeuronAllocTest, MemsetBasic) {
    void *addr = allocAndTrack(4096);
    ASSERT_EQ(neuronMemset(addr, 0x42, 4096), 0);

    std::vector<uint8_t> dst(4096, 0);
    ASSERT_EQ(neuronMemcpy(dst.data(), addr, 4096, neuronMemcpyDeviceToHost), 0);
    for (auto b : dst) {
        ASSERT_EQ(b, 0x42);
    }
}

TEST_F(NeuronAllocTest, MemsetAtOffset) {
    constexpr size_t alloc_size = 8192;
    constexpr size_t offset = 2048;
    constexpr size_t set_len = 1024;

    void *base = allocAndTrack(alloc_size);
    void *mid = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(base) + offset);

    ASSERT_EQ(neuronMemset(mid, 0x77, set_len), 0);

    std::vector<uint8_t> dst(set_len, 0);
    ASSERT_EQ(neuronMemcpy(dst.data(), mid, set_len, neuronMemcpyDeviceToHost), 0);
    for (auto b : dst) {
        ASSERT_EQ(b, 0x77);
    }
}

TEST_F(NeuronAllocTest, MemsetOutOfBounds) {
    void *addr = allocAndTrack(1024);
    ASSERT_EQ(neuronMemset(addr, 0xFF, 2048), -1);
}

TEST_F(NeuronAllocTest, MemsetUnknownAddress) {
    void *bogus = reinterpret_cast<void *>(static_cast<uintptr_t>(0xDEADBEEF));
    ASSERT_EQ(neuronMemset(bogus, 0xFF, 64), -1);
}

TEST_F(NeuronAllocTest, MultipleAllocationsRangeLookup) {
    // Allocate multiple buffers and verify range lookup works for each.
    constexpr size_t num_allocs = 4;
    constexpr size_t alloc_size = 4096;
    void *addrs[num_allocs];

    for (size_t i = 0; i < num_allocs; i++) {
        addrs[i] = allocAndTrack(alloc_size);
    }

    // Write a distinct pattern into the middle of each allocation.
    for (size_t i = 0; i < num_allocs; i++) {
        void *mid = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(addrs[i]) + 2048);
        std::vector<uint8_t> pattern(128, static_cast<uint8_t>(i + 1));
        ASSERT_EQ(neuronMemcpy(mid, pattern.data(), 128, neuronMemcpyHostToDevice), 0);
    }

    // Read back and verify.
    for (size_t i = 0; i < num_allocs; i++) {
        void *mid = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(addrs[i]) + 2048);
        std::vector<uint8_t> buf(128, 0);
        ASSERT_EQ(neuronMemcpy(buf.data(), mid, 128, neuronMemcpyDeviceToHost), 0);
        for (auto b : buf) {
            ASSERT_EQ(b, static_cast<uint8_t>(i + 1));
        }
    }
}

TEST_F(NeuronAllocTest, AdjacentAllocationsNoOverlap) {
    // Verify that a VA just past one allocation doesn't match it.
    void *addr = allocAndTrack(4096);
    void *just_past = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(addr) + 4096);
    std::vector<uint8_t> buf(64, 0);
    // This should fail — just_past is outside the allocation.
    ASSERT_EQ(neuronMemcpy(just_past, buf.data(), 64, neuronMemcpyHostToDevice), -1);
}

TEST_F(NeuronAllocTest, LargeMemsetChunked) {
    // neuronMemset uses 2MB chunks internally; verify it works for > 2MB.
    constexpr size_t size = (1UL << 21) + 4096; // 2MB + 4KB
    void *addr = allocAndTrack(size);
    ASSERT_EQ(neuronMemset(addr, 0xAA, size), 0);

    std::vector<uint8_t> dst(size, 0);
    ASSERT_EQ(neuronMemcpy(dst.data(), addr, size, neuronMemcpyDeviceToHost), 0);
    for (size_t i = 0; i < size; i++) {
        ASSERT_EQ(dst[i], 0xAA) << "mismatch at byte " << i;
    }
}

TEST_F(NeuronAllocTest, NeuronCoreCount) {
    // With mock NRT, should return 2 (mock returns vnc_count=2).
    ASSERT_EQ(neuronCoreCount(), 2);
}
