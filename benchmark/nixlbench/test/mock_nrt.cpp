/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Amazon.com, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */

// Mock NRT library for unit testing neuron.cpp without a Neuron device.
// Compiled as libnrt.so.1 and found via LD_LIBRARY_PATH so neuron.cpp's
// dlopen("libnrt.so.1") resolves to it. Tensors are backed by host memory.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

struct nrt_tensor {
    void *va;
    size_t size;
    unsigned char *data;
};

extern "C" {

// Records the vnc index passed to the most recent nrt_tensor_allocate() so
// tests can assert neuronMalloc()'s SG-vs-MG vnc selection.
int nixlbench_test_last_vnc = -1;

int
nrt_init(int, const char *, const char *) {
    return 0;
}

int
nrt_tensor_allocate(int, int vnc, size_t size, const char *, nrt_tensor **tensor) {
    nixlbench_test_last_vnc = vnc;
    auto *t = new nrt_tensor;
    t->size = size;
    t->data = new unsigned char[size];
    memset(t->data, 0, size);
    t->va = t->data;
    *tensor = t;
    return 0;
}

void
nrt_tensor_free(nrt_tensor **tensor) {
    if (tensor && *tensor) {
        delete[] (*tensor)->data;
        delete *tensor;
        *tensor = nullptr;
    }
}

int
nrt_tensor_read(const nrt_tensor *tensor, void *buf, size_t offset, size_t size) {
    if (!tensor || offset + size > tensor->size) return -1;
    memcpy(buf, tensor->data + offset, size);
    return 0;
}

int
nrt_tensor_write(nrt_tensor *tensor, const void *buf, size_t offset, size_t size) {
    if (!tensor || offset + size > tensor->size) return -1;
    memcpy(tensor->data + offset, buf, size);
    return 0;
}

void *
nrt_tensor_get_va(const nrt_tensor *tensor) {
    if (!tensor) return nullptr;
    return tensor->va;
}

int
nrt_get_visible_vnc_count(uint32_t *vnc_count) {
    *vnc_count = 2;
    return 0;
}

} // extern "C"
