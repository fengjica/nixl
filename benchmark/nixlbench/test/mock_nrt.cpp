/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Amazon.com, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */

// Mock NRT library for unit testing neuron.cpp allocation tracking.
// Compiled as a shared library and loaded via LD_PRELOAD or dlopen path override.
// Tensors are backed by plain host memory.

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

int
nrt_init(int, const char *, const char *) {
    return 0;
}

int
nrt_tensor_allocate(int, int, size_t size, const char *, nrt_tensor **tensor) {
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
        delete[](*tensor)->data;
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
