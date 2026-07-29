/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 Amazon.com, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "libfabric_fi_more_window.h"

FiMoreWindow::FiMoreWindow(int batch_size, size_t num_rails, size_t num_remote_eps, int lookahead)
    : batch_size_(batch_size),
      num_remote_eps_(num_remote_eps),
      last_desc_idx_(num_rails * num_remote_eps, -1),
      // The window spans the current descriptor through the furthest one admitted ahead of it, so
      // lookahead + 1 slots suffice for those to never collide.
      rail_((size_t)lookahead + 1, -1),
      remote_ep_((size_t)lookahead + 1, -1),
      posts_since_flush_(num_rails, 0) {}

void
FiMoreWindow::reset() {
    last_desc_idx_.assign(last_desc_idx_.size(), -1);
    rail_.assign(rail_.size(), -1);
    remote_ep_.assign(remote_ep_.size(), -1);
    posts_since_flush_.assign(posts_since_flush_.size(), 0);
}

void
FiMoreWindow::addAhead(int desc_idx, int rail_id, int remote_ep_id) {
    // A descriptor missing either id is not batched. Normalize both to -1 here, so a single rail_
    // test decides that everywhere downstream.
    const bool batched = rail_id >= 0 && remote_ep_id >= 0;
    rail_[slot(desc_idx)] = batched ? rail_id : -1;
    remote_ep_[slot(desc_idx)] = batched ? remote_ep_id : -1;
    if (batched) {
        last_desc_idx_[cell(rail_id, remote_ep_id)] = desc_idx;
    }
}

bool
FiMoreWindow::useFiMore(int desc_idx) {
    const int rail_id = rail_[slot(desc_idx)];
    if (rail_id < 0) {
        // Not batched, so it holds no window entry to consume.
        return false;
    }
    // is_last covers both cases that would otherwise strand WQEs: the batch moving to another
    // endpoint on this rail, and the rail's last post in the chunk.
    const bool is_last = consumeCurrent(desc_idx, rail_id);
    if (is_last || posts_since_flush_[rail_id] == batch_size_ - 1) {
        posts_since_flush_[rail_id] = 0;
        return false;
    }
    ++posts_since_flush_[rail_id];
    return true;
}

bool
FiMoreWindow::consumeCurrent(int desc_idx, int rail_id) {
    // The pair's entry is >= desc_idx: this descriptor's own addAhead set it, and only later
    // descriptors raise it. So equality means no descriptor further ahead in the window shares the
    // pair, and this post must flush. Clearing the entry then leaves the pair absent, which is what
    // a subsequent addAhead beyond the horizon expects.
    int &last = last_desc_idx_[cell(rail_id, remote_ep_[slot(desc_idx)])];
    if (last == desc_idx) {
        last = -1;
        return true;
    }
    return false;
}
