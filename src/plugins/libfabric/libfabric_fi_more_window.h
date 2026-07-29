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
#ifndef NIXL_SRC_PLUGINS_LIBFABRIC_LIBFABRIC_FI_MORE_WINDOW_H
#define NIXL_SRC_PLUGINS_LIBFABRIC_LIBFABRIC_FI_MORE_WINDOW_H

#include <cstddef>
#include <vector>

/** Decides which descriptors of a WRITE posting loop carry FI_MORE.
 *
 *  A FI_MORE post rings no doorbell, so a rail's queued WQEs are only submitted by a later
 *  non-FI_MORE post on that rail. This window slides over the descriptors about to be posted and
 *  tracks the last index each (rail, remote endpoint) pair appears at, so a rail's batch is
 *  flushed as soon as nothing further ahead will reuse the pair. That bounds how long queued WQEs
 *  wait to at most `lookahead` descriptors, and caps a batch at `batch_size` posts.
 *
 *  This is a plain bookkeeping structure -- it holds no engine, connection, or descriptor-list
 *  state. The caller resolves each descriptor's rail and endpoint once (when it enters the window
 *  via addAhead) and feeds them in, so a full walk costs O(descriptors) rather than
 *  O(descriptors * window).
 *
 *  Not thread safe: one window drives a single posting pass on a single thread.
 */
class FiMoreWindow {
public:
    /** All dimensions are fixed for the window's lifetime, so its storage is allocated once here
     *  and only cleared between posting passes.
     *  @param batch_size     max posts a rail may queue with FI_MORE before it must flush
     *  @param num_rails      bounds local rail ids (each in [0, num_rails))
     *  @param num_remote_eps bounds remote endpoint ids (each in [0, num_remote_eps))
     *  @param lookahead      how many descriptors ahead of the current one the window admits */
    FiMoreWindow(int batch_size, size_t num_rails, size_t num_remote_eps, int lookahead);

    /** Clear all state for a fresh posting pass. */
    void
    reset();

    /** Admit a descriptor at the lookahead frontier, recording it as the latest index of its
     *  (rail, endpoint) pair. Either id may be -1 when the descriptor is not part of FI_MORE
     *  batching; this is the only place that is checked. */
    void
    addAhead(int desc_idx, int rail_id, int remote_ep_id);

    /** Whether descriptor desc_idx should carry FI_MORE: false (flush) at the last post to its
     *  (rail, endpoint) pair within the window -- which covers both a batch moving to another
     *  endpoint and a rail's last post -- and when the rail's batch reaches batch_size. A
     *  descriptor that is not batched never carries FI_MORE.
     *
     *  Consumes desc_idx from the window, so call once per descriptor, in posting order. */
    bool
    useFiMore(int desc_idx);

private:
    /** Take a batched descriptor out of the window, returning whether it is the last post to its
     *  (rail, endpoint) pair within the window. rail_id must be the descriptor's rail. */
    bool
    consumeCurrent(int desc_idx, int rail_id);

    size_t
    slot(int desc_idx) const {
        return (size_t)desc_idx % rail_.size();
    }

    size_t
    cell(int rail_id, int remote_ep_id) const {
        return (size_t)rail_id * num_remote_eps_ + (size_t)remote_ep_id;
    }

    const int batch_size_;
    const size_t num_remote_eps_;
    // Last descriptor index per (rail, endpoint) pair; -1 when the pair is not in the window.
    std::vector<int> last_desc_idx_;
    // Ring buffers indexed by desc_idx % capacity, so consuming a descriptor needs no search.
    std::vector<int> rail_;
    std::vector<int> remote_ep_;
    // Posts each rail has queued with FI_MORE since its last flush.
    std::vector<int> posts_since_flush_;
};

#endif // NIXL_SRC_PLUGINS_LIBFABRIC_LIBFABRIC_FI_MORE_WINDOW_H
