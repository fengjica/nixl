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

#include "libfabric_post_profile.h"

#include <fnmatch.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "common/nixl_log.h"

namespace nixl::libfabric::postProfile {

mask_t selected_mask = 0;
double ns_per_tick = 0.0;
bool use_tsc = false;
thread_local nixlPostPhaseSamples thread_samples;

namespace {

// The unitless counters and the calibration series are enabled whenever anything
// is: they are integer adds, not timestamps, and without them a phase number has
// no context (how many requests went out, how many of them hit backpressure).
constexpr nixl_telemetry_event_type_t always_on_events[] = {
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_CALIBRATION,
    nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_ATTEMPTS,
    nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_MAX_ATTEMPTS,
    nixl_telemetry_event_type_t::AGENT_POST_SUBMITTED_REQUESTS,
    nixl_telemetry_event_type_t::AGENT_POST_RAILS_TOUCHED,
};

// A phase's short selector name: its telemetry event name with the family prefix
// stripped, so a run can ask for 'submit_loop' rather than
// 'agent_post_phase_submit_loop'.
[[nodiscard]] std::string
shortName(nixl_telemetry_event_type_t type) {
    const std::string name{nixlEnumStrings::telemetryEventTypeStr(type)};
    for (const char *prefix : {"agent_post_phase_", "agent_post_accum_", "agent_post_"}) {
        if (name.rfind(prefix, 0) == 0) {
            return name.substr(std::strlen(prefix));
        }
    }
    return name;
}

// The rest of src/utils/libfabric reads its environment with plain getenv (see
// getCustomStringParam in libfabric_common.cpp); this layer deliberately does not
// depend on nixl::config, and a debug-only profiling knob is not the place to
// change that.
[[nodiscard]] std::optional<std::string>
readEnv(const char *name) {
    if (const char *value = std::getenv(name)) {
        return std::string(value);
    }
    return std::nullopt;
}

// Comma-separated selector list, whitespace trimmed, empty entries dropped.
// A set rather than a vector so 'submit_loop,submit_loop' is not reported twice.
[[nodiscard]] std::set<std::string>
splitTokens(const std::string &spec) {
    std::set<std::string> tokens;
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t end = std::min(spec.find(',', pos), spec.size());
        size_t begin = pos;
        size_t last = end;
        while (begin < last && std::isspace(static_cast<unsigned char>(spec[begin]))) {
            ++begin;
        }
        while (last > begin && std::isspace(static_cast<unsigned char>(spec[last - 1]))) {
            --last;
        }
        if (last > begin) {
            tokens.emplace(spec.substr(begin, last - begin));
        }
        pos = end + 1;
    }
    return tokens;
}

[[nodiscard]] mask_t
parseSelector(const std::string &spec) {
    const std::set<std::string> tokens = splitTokens(spec);
    if (tokens.empty()) {
        return 0;
    }

    mask_t mask = 0;
    std::set<std::string> unmatched(tokens);
    for (size_t i = 0; i < nixl_post_phase_event_count; ++i) {
        const auto type = static_cast<nixl_telemetry_event_type_t>(
            static_cast<size_t>(nixl_post_phase_first_event) + i);
        const std::string full{nixlEnumStrings::telemetryEventTypeStr(type)};
        const std::string brief = shortName(type);
        for (const auto &token : tokens) {
            if (token == "all" || fnmatch(token.c_str(), full.c_str(), 0) == 0 ||
                fnmatch(token.c_str(), brief.c_str(), 0) == 0) {
                mask |= mask_t{1} << i;
                unmatched.erase(token);
            }
        }
    }

    for (const auto &token : unmatched) {
        NIXL_WARN << "Ignoring " << POST_PROFILE_VAR << " entry '" << token
                  << "': no postXfer phase matches";
    }
    return mask;
}

// Only reachable on architectures where readClock() can use rdtsc; defining them
// unconditionally would warn as unused everywhere else (aarch64 included).
#ifdef NIXL_POST_PROFILE_HAVE_RDTSC

// rdtsc is only a usable clock if the kernel itself trusts the TSC. When the
// clocksource is hpet or acpi_pm the TSC is not guaranteed constant or
// synchronized across cores, and a phase timed across a migration would be
// garbage rather than merely noisy.
[[nodiscard]] bool
kernelUsesTsc() {
    std::ifstream source("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    if (!source) {
        // Not Linux, or the sysfs entry is unavailable: do not assume.
        return false;
    }
    std::string name;
    source >> name;
    return name == "tsc";
}

// Nanoseconds per TSC tick, measured against steady_clock. 20 ms is long enough
// that the two clocks' read overhead is negligible against the interval, and
// short enough to be unnoticeable at engine construction.
[[nodiscard]] double
calibrateNsPerTick() {
    const auto wall_start = std::chrono::steady_clock::now();
    const ticks_t tsc_start = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const ticks_t tsc_end = __rdtsc();
    const auto wall_end = std::chrono::steady_clock::now();

    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count();
    const ticks_t elapsed_ticks = tsc_end - tsc_start;
    if (elapsed_ticks == 0 || elapsed_ns <= 0) {
        return 0.0;
    }
    return static_cast<double>(elapsed_ns) / static_cast<double>(elapsed_ticks);
}

#endif // NIXL_POST_PROFILE_HAVE_RDTSC

[[nodiscard]] std::string
selectedNames(mask_t mask) {
    std::string result;
    for (size_t i = 0; i < nixl_post_phase_event_count; ++i) {
        if ((mask & (mask_t{1} << i)) == 0) {
            continue;
        }
        const auto type = static_cast<nixl_telemetry_event_type_t>(
            static_cast<size_t>(nixl_post_phase_first_event) + i);
        if (!result.empty()) {
            result += ',';
        }
        result += nixlEnumStrings::telemetryEventTypeStr(type);
    }
    return result;
}

std::once_flag init_flag;
std::once_flag thread_pool_warn_flag;

void
initOnce() {
    const auto spec = readEnv(POST_PROFILE_VAR);
    if (!spec) {
        return;
    }

    mask_t mask = parseSelector(*spec);
    if (mask == 0) {
        NIXL_WARN << POST_PROFILE_VAR << "='" << *spec
                  << "' selected no postXfer phase; profiling stays off";
        return;
    }
    for (const auto event : always_on_events) {
        mask |= mask_t{1} << nixlPostPhaseSamples::index(event);
    }

#ifdef NIXL_POST_PROFILE_HAVE_RDTSC
    if (kernelUsesTsc()) {
        ns_per_tick = calibrateNsPerTick();
        use_tsc = ns_per_tick > 0.0;
        if (!use_tsc) {
            NIXL_WARN << "TSC calibration produced no usable rate; postXfer profiling will use "
                         "steady_clock, which costs ~20-25ns per timestamp instead of ~6-8ns";
        }
    } else {
        NIXL_WARN << "Kernel clocksource is not 'tsc'; postXfer profiling will use steady_clock, "
                     "which costs ~20-25ns per timestamp instead of ~6-8ns";
    }
#endif

    // Published last: this is what turns the hooks on, and everything they read
    // must already be in place.
    selected_mask = mask;

    NIXL_INFO << "postXfer profiling active (" << POST_PROFILE_VAR << "='" << *spec
              << "'), phases=[" << selectedNames(selected_mask) << "], clock="
              << (use_tsc ? "rdtsc" : "steady_clock");
    if (use_tsc) {
        NIXL_DEBUG << "postXfer profiling TSC rate: " << ns_per_tick << " ns/tick";
    }
}

} // namespace

void
initFromEnv() {
    std::call_once(init_flag, initOnce);
}

void
warnThreadPoolActive() {
    std::call_once(thread_pool_warn_flag, []() {
        NIXL_WARN << "postXfer profiling is active but the post thread pool is splitting this "
                     "transfer across threads. Per-descriptor samples taken on pool threads are "
                     "never drained, so the reported phases cover only the calling thread's share. "
                     "The methodology assumes the single-thread path: set the post thread count to "
                     "0 for a measurement run.";
    });
}

} // namespace nixl::libfabric::postProfile
