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
#ifndef NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_POST_PROFILE_H
#define NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_POST_PROFILE_H

#include <chrono>
#include <cstdint>
#include <string>

#include "backend/backend_engine.h"
#include "telemetry_event.h"

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define NIXL_POST_PROFILE_HAVE_RDTSC 1
#endif

// postXfer-path profiling harness for the libfabric backend.
// See docs/profiling-postxfer-telemetry.md for the methodology this implements.
//
// Every design choice here follows from the per-post budget: sustaining 40 GB/s
// with 16 KB messages needs a post every ~400 ns, so the instrument is a
// significant fraction of the interval being measured unless it is kept cheap.
//
//  - Timestamps use rdtsc (~6-8 ns) instead of steady_clock::now() (~20-25 ns
//    through the vDSO) where the TSC is usable, and fall back to steady_clock
//    where it is not.
//  - An unselected phase takes no timestamp at all: a load, a bit test, and a
//    well-predicted branch. NIXL_POST_PROFILE names the phase for a run, so the
//    number that comes out is not the phase plus the cost of timing every other
//    phase.
//  - The per-descriptor accumulators fire once per descriptor, so they add into a
//    fixed thread_local POD and are exported once per postXfer. Anything O(batch)
//    on this path would manufacture the batch-size trend under investigation.
//  - Every run also reports agent_post_phase_calibration, the live cost of one
//    timestamp pair, so a phase number can be read against the noise floor of the
//    instrument that produced it.
//
// Thread safety: the selector and clock calibration are written once by
// initFromEnv() at engine construction, before any posting thread exists, and are
// read-only thereafter. The accumulator is thread_local and is drained by the
// agent on the same thread that ran postXfer.

namespace nixl::libfabric::postProfile {

constexpr char POST_PROFILE_VAR[] = "NIXL_POST_PROFILE";

using ticks_t = uint64_t;

// One bit per phase event, indexed exactly as nixlPostPhaseSamples is.
using mask_t = uint32_t;
static_assert(nixl_post_phase_event_count <= 32,
              "postProfile::mask_t needs a bit per postXfer phase event");

// Deliberately plain globals rather than function-local statics: isSelected() is
// on the post path of every run, profiling or not, and a function-local static
// would put a thread-safe-initialization guard load and branch there forever.
// Zero-initialized, so profiling is off until initFromEnv() turns it on.
extern mask_t selected_mask;
// Nanoseconds per TSC tick, from the calibration in initFromEnv().
extern double ns_per_tick;
// True only if rdtsc is available and the kernel's clocksource is the TSC. When
// false, readClock() uses steady_clock.
extern bool use_tsc;

// The per-thread sample block. Not static/function-local for the same reason as
// above; a thread_local accessor function would add a TLS-wrapper call.
extern thread_local nixlPostPhaseSamples thread_samples;

// Parse NIXL_POST_PROFILE and, if anything was selected, calibrate the clock.
// Call once per process from the engine constructor; later calls are no-ops.
//
// The value is a comma-separated glob list matched against both the full
// telemetry event names (agent_post_phase_submit_loop) and their short forms
// (submit_loop), so 'submit_loop', 'agent_post_phase_submit_loop' and '*submit*'
// all select the same phase. 'all' or '*' selects everything, which is useful for
// a smoke test but not for measurement -- see the one-phase-per-run rationale in
// the doc. The unitless counters are always enabled when anything is, since they
// are integer increments and give every run its context.
void
initFromEnv();

// Whether any postXfer profiling is active. This is the guard on every hook
// below, so a build with profiling unused pays one load and one branch per post.
[[nodiscard]] inline bool
enabled() noexcept {
    return selected_mask != 0;
}

[[nodiscard]] inline bool
isSelected(nixl_telemetry_event_type_t phase) noexcept {
    return (selected_mask & (mask_t{1} << nixlPostPhaseSamples::index(phase))) != 0;
}

[[nodiscard]] inline ticks_t
readClock() noexcept {
#ifdef NIXL_POST_PROFILE_HAVE_RDTSC
    if (use_tsc) {
        return __rdtsc();
    }
#endif
    return static_cast<ticks_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

[[nodiscard]] inline uint64_t
elapsedNs(ticks_t start, ticks_t end) noexcept {
    const ticks_t delta = end - start;
#ifdef NIXL_POST_PROFILE_HAVE_RDTSC
    if (use_tsc) {
        return static_cast<uint64_t>(static_cast<double>(delta) * ns_per_tick);
    }
#endif
    // steady_clock's period is not guaranteed to be nanoseconds, so convert
    // rather than assuming.
    const std::chrono::steady_clock::duration span{
        static_cast<std::chrono::steady_clock::rep>(delta)};
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(span).count());
}

// Times one phase for as long as it is in scope, and only if that phase was
// selected for this run. An unselected phase reads no clock.
class scopedPhase {
public:
    explicit scopedPhase(nixl_telemetry_event_type_t phase) noexcept
        : phase_(phase),
          active_(isSelected(phase)),
          start_(active_ ? readClock() : 0) {}

    scopedPhase(const scopedPhase &) = delete;
    scopedPhase &
    operator=(const scopedPhase &) = delete;

    ~scopedPhase() {
        if (active_) {
            thread_samples.add(phase_, elapsedNs(start_, readClock()));
        }
    }

private:
    const nixl_telemetry_event_type_t phase_;
    const bool active_;
    const ticks_t start_;
};

// Add to a unitless counter (EAGAIN attempts, submitted requests, rails touched).
inline void
count(nixl_telemetry_event_type_t event, uint64_t n) noexcept {
    if (enabled()) {
        thread_samples.add(event, n);
    }
}

// Raise a high-water mark (the per-post EAGAIN retry maximum).
inline void
observeMax(nixl_telemetry_event_type_t event, uint64_t value) noexcept {
    if (enabled()) {
        thread_samples.setMax(event, value);
    }
}

// Start a post: discard anything left over. The agent's drain normally empties
// the block, but this keeps each post's samples independent of whether telemetry
// was configured to drain at all.
inline void
beginPost() noexcept {
    if (enabled()) {
        thread_samples.clear();
    }
}

// Finish a post: record what one timestamp pair cost, measured live rather than
// once at startup so the number tracks core migration and frequency changes over
// the run.
inline void
endPost() noexcept {
    if (!enabled()) {
        return;
    }
    const ticks_t first = readClock();
    const ticks_t second = readClock();
    thread_samples.add(nixl_telemetry_event_type_t::AGENT_POST_PHASE_CALIBRATION,
                       elapsedNs(first, second));
}

// Brackets one postXfer call. Declare it first in postXfer so that beginPost()
// and endPost() run on every path out, including the error returns.
class scopedPost {
public:
    scopedPost() noexcept {
        beginPost();
    }

    scopedPost(const scopedPost &) = delete;
    scopedPost &
    operator=(const scopedPost &) = delete;

    ~scopedPost() {
        endPost();
    }
};

// Hand the accumulated samples to the caller and reset. Backs
// nixlBackendEngine::drainPostPhaseSamples().
[[nodiscard]] inline bool
drain(nixlPostPhaseSamples &out) noexcept {
    if (!enabled() || thread_samples.empty()) {
        return false;
    }
    out = thread_samples;
    thread_samples.clear();
    return true;
}

// Warn (once) that the post thread pool splits a transfer across threads, so the
// submit-loop and per-descriptor samples taken on pool threads are never drained
// and the reported phases cover only the posting thread's share. The methodology
// targets the single-thread path, so this is a misconfiguration rather than a
// case to support.
void
warnThreadPoolActive();

} // namespace nixl::libfabric::postProfile

#endif // NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_POST_PROFILE_H
