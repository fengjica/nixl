/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
#ifndef NIXL_SRC_CORE_TELEMETRY_TELEMETRY_EVENT_H
#define NIXL_SRC_CORE_TELEMETRY_TELEMETRY_EVENT_H

#include <array>
#include <cstdint>
#include <string_view>

#include "nixl_types.h"

constexpr char TELEMETRY_BUFFER_SIZE_VAR[] = "NIXL_TELEMETRY_BUFFER_SIZE";
constexpr char TELEMETRY_RUN_INTERVAL_VAR[] = "NIXL_TELEMETRY_RUN_INTERVAL";
constexpr char TELEMETRY_ENABLED_METRICS_VAR[] = "NIXL_TELEMETRY_ENABLED_METRICS";

// Bumped 4 -> 5 when the postXfer-path profiling events were inserted ahead of
// AGENT_TELEMETRY_EVENTS_DROPPED, which changed that enumerator's wire value.
constexpr inline int TELEMETRY_VERSION = 5;

/**
 * @enum nixl_telemetry_event_type_t
 * @brief Enumerates all known telemetry event types.
 */
enum class nixl_telemetry_event_type_t : uint32_t {
    AGENT_TX_BYTES = 0,
    AGENT_RX_BYTES = 1,
    AGENT_TX_REQUESTS_NUM = 2,
    AGENT_RX_REQUESTS_NUM = 3,
    AGENT_MEMORY_REGISTERED = 4,
    AGENT_MEMORY_DEREGISTERED = 5,
    AGENT_XFER_TIME = 6,
    AGENT_XFER_POST_TIME = 7,
    AGENT_ERR_NOT_POSTED = 8,
    AGENT_ERR_INVALID_PARAM = 9,
    AGENT_ERR_BACKEND = 10,
    AGENT_ERR_NOT_FOUND = 11,
    AGENT_ERR_MISMATCH = 12,
    AGENT_ERR_NOT_ALLOWED = 13,
    AGENT_ERR_REPOST_ACTIVE = 14,
    AGENT_ERR_UNKNOWN = 15,
    AGENT_ERR_NOT_SUPPORTED = 16,
    AGENT_ERR_REMOTE_DISCONNECT = 17,
    AGENT_ERR_CANCELED = 18,
    AGENT_ERR_NO_TELEMETRY = 19,

    // --- postXfer-path profiling (docs/profiling-postxfer-telemetry.md) ---
    // Produced by backends via nixlBackendEngine::drainPostPhaseSamples() and
    // published once per posted transfer. All timing events below carry
    // NANOSECONDS, not microseconds: the phases being resolved are individually
    // sub-microsecond, so a us-truncated integer would read as zero. The
    // exporters scale them into the shared microsecond histogram buckets via
    // nixlTelemetryMetricDescriptor::histogramScaleToUs.
    //
    // Phases timed once per postXfer, outside the per-descriptor loop.
    AGENT_POST_PHASE_CONN_LOOKUP = 20,
    AGENT_POST_PHASE_NOTIF_PREP = 21,
    AGENT_POST_PHASE_MD_VALIDATE = 22,
    AGENT_POST_PHASE_FI_MORE_PREPASS = 23,
    AGENT_POST_PHASE_SUBMIT_LOOP = 24,
    AGENT_POST_PHASE_NOTIF_SEND = 25,
    AGENT_POST_PHASE_PROGRESS_TAIL = 26,
    // Per-descriptor costs summed across the loop, emitted once per postXfer.
    AGENT_POST_ACCUM_RAIL_SELECT = 27,
    AGENT_POST_ACCUM_REQ_ALLOC = 28,
    AGENT_POST_ACCUM_FI_WRITEMSG = 29,
    AGENT_POST_ACCUM_EAGAIN_DRAIN = 30,
    AGENT_POST_ACCUM_CUDA_CTX = 31,
    // Cost of the instrumentation harness itself, measured over a zero-length
    // region. Subtract it from every other phase in the same run.
    AGENT_POST_PHASE_CALIBRATION = 32,
    // Unitless counters: no timestamps taken, so these stay affordable unsampled.
    AGENT_POST_EAGAIN_ATTEMPTS = 33,
    AGENT_POST_EAGAIN_MAX_ATTEMPTS = 34,
    AGENT_POST_SUBMITTED_REQUESTS = 35,
    AGENT_POST_RAILS_TOUCHED = 36,

    AGENT_TELEMETRY_EVENTS_DROPPED = 37,
};

// Number of AGENT_POST_* profiling events, and the value of the first one. The
// backend-side sample block is indexed by (event - first), so these two must
// stay in step with the enum block above.
inline constexpr std::size_t nixl_post_phase_event_count = 17;
inline constexpr nixl_telemetry_event_type_t nixl_post_phase_first_event =
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_CONN_LOOKUP;

inline constexpr std::size_t nixl_telemetry_event_type_count =
    static_cast<std::size_t>(nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED) + 1;

// Per-event-type flag mask indexed by nixl_telemetry_event_type_t.
using nixl_telemetry_metric_mask_t = std::array<bool, nixl_telemetry_event_type_count>;

[[nodiscard]] nixl_telemetry_event_type_t
nixlTelemetryEventTypeForStatus(nixl_status_t s);

inline constexpr std::array telemetry_error_event_types = {
    nixl_telemetry_event_type_t::AGENT_ERR_NOT_POSTED,
    nixl_telemetry_event_type_t::AGENT_ERR_INVALID_PARAM,
    nixl_telemetry_event_type_t::AGENT_ERR_BACKEND,
    nixl_telemetry_event_type_t::AGENT_ERR_NOT_FOUND,
    nixl_telemetry_event_type_t::AGENT_ERR_MISMATCH,
    nixl_telemetry_event_type_t::AGENT_ERR_NOT_ALLOWED,
    nixl_telemetry_event_type_t::AGENT_ERR_REPOST_ACTIVE,
    nixl_telemetry_event_type_t::AGENT_ERR_UNKNOWN,
    nixl_telemetry_event_type_t::AGENT_ERR_NOT_SUPPORTED,
    nixl_telemetry_event_type_t::AGENT_ERR_REMOTE_DISCONNECT,
    nixl_telemetry_event_type_t::AGENT_ERR_CANCELED,
    nixl_telemetry_event_type_t::AGENT_ERR_NO_TELEMETRY,
};

inline constexpr std::array telemetry_metric_event_types = {
    nixl_telemetry_event_type_t::AGENT_TX_BYTES,
    nixl_telemetry_event_type_t::AGENT_RX_BYTES,
    nixl_telemetry_event_type_t::AGENT_TX_REQUESTS_NUM,
    nixl_telemetry_event_type_t::AGENT_RX_REQUESTS_NUM,
    nixl_telemetry_event_type_t::AGENT_MEMORY_REGISTERED,
    nixl_telemetry_event_type_t::AGENT_MEMORY_DEREGISTERED,
    nixl_telemetry_event_type_t::AGENT_XFER_TIME,
    nixl_telemetry_event_type_t::AGENT_XFER_POST_TIME,
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_CONN_LOOKUP,
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_NOTIF_PREP,
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_MD_VALIDATE,
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_FI_MORE_PREPASS,
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_SUBMIT_LOOP,
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_NOTIF_SEND,
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_PROGRESS_TAIL,
    nixl_telemetry_event_type_t::AGENT_POST_ACCUM_RAIL_SELECT,
    nixl_telemetry_event_type_t::AGENT_POST_ACCUM_REQ_ALLOC,
    nixl_telemetry_event_type_t::AGENT_POST_ACCUM_FI_WRITEMSG,
    nixl_telemetry_event_type_t::AGENT_POST_ACCUM_EAGAIN_DRAIN,
    nixl_telemetry_event_type_t::AGENT_POST_ACCUM_CUDA_CTX,
    nixl_telemetry_event_type_t::AGENT_POST_PHASE_CALIBRATION,
    nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_ATTEMPTS,
    nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_MAX_ATTEMPTS,
    nixl_telemetry_event_type_t::AGENT_POST_SUBMITTED_REQUESTS,
    nixl_telemetry_event_type_t::AGENT_POST_RAILS_TOUCHED,
    nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED,
};

static_assert(nixl_telemetry_event_type_count ==
                  telemetry_metric_event_types.size() + telemetry_error_event_types.size(),
              "AGENT_TELEMETRY_EVENTS_DROPPED must remain the last enumerator; "
              "nixl_telemetry_event_type_count is out of sync with the event-type enum");

// The error events share one family, so they have no per-type descriptor row.
inline constexpr const char *telemetry_error_family_name = "agent_errors_total";
inline constexpr const char *telemetry_error_family_help = "Cumulative error count by status";

struct nixlTelemetryMetricDescriptor {
    const char *counterName;
    const char *counterHelp;
    const char *gaugeName;
    const char *gaugeHelp;
    const char *histogramName;
    const char *histogramHelp;
    // Factor converting this event's raw value into the microseconds that the
    // shared histogram bucket bounds (NIXL_TELEMETRY_HISTOGRAM_BUCKETS_US) are
    // expressed in. Events already denominated in microseconds leave it at 1.0;
    // the nanosecond-valued AGENT_POST_* phases set 0.001. Exporters must apply
    // it before Observe(), otherwise ns values land 1000x off in us buckets.
    // Only meaningful when histogramName != nullptr.
    double histogramScaleToUs = 1.0;
};

namespace nixlEnumStrings {
[[nodiscard]] constexpr std::string_view
telemetryEventTypeStr(const nixl_telemetry_event_type_t type) noexcept {
    switch (type) {
    case nixl_telemetry_event_type_t::AGENT_TX_BYTES:
        return "agent_tx_bytes";
    case nixl_telemetry_event_type_t::AGENT_RX_BYTES:
        return "agent_rx_bytes";
    case nixl_telemetry_event_type_t::AGENT_TX_REQUESTS_NUM:
        return "agent_tx_requests_num";
    case nixl_telemetry_event_type_t::AGENT_RX_REQUESTS_NUM:
        return "agent_rx_requests_num";
    case nixl_telemetry_event_type_t::AGENT_MEMORY_REGISTERED:
        return "agent_memory_registered";
    case nixl_telemetry_event_type_t::AGENT_MEMORY_DEREGISTERED:
        return "agent_memory_deregistered";
    case nixl_telemetry_event_type_t::AGENT_XFER_TIME:
        return "agent_xfer_time";
    case nixl_telemetry_event_type_t::AGENT_XFER_POST_TIME:
        return "agent_xfer_post_time";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_POSTED:
        return "agent_err_not_posted";
    case nixl_telemetry_event_type_t::AGENT_ERR_INVALID_PARAM:
        return "agent_err_invalid_param";
    case nixl_telemetry_event_type_t::AGENT_ERR_BACKEND:
        return "agent_err_backend";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_FOUND:
        return "agent_err_not_found";
    case nixl_telemetry_event_type_t::AGENT_ERR_MISMATCH:
        return "agent_err_mismatch";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_ALLOWED:
        return "agent_err_not_allowed";
    case nixl_telemetry_event_type_t::AGENT_ERR_REPOST_ACTIVE:
        return "agent_err_repost_active";
    case nixl_telemetry_event_type_t::AGENT_ERR_UNKNOWN:
        return "agent_err_unknown";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_SUPPORTED:
        return "agent_err_not_supported";
    case nixl_telemetry_event_type_t::AGENT_ERR_REMOTE_DISCONNECT:
        return "agent_err_remote_disconnect";
    case nixl_telemetry_event_type_t::AGENT_ERR_CANCELED:
        return "agent_err_canceled";
    case nixl_telemetry_event_type_t::AGENT_ERR_NO_TELEMETRY:
        return "agent_err_no_telemetry";
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_CONN_LOOKUP:
        return "agent_post_phase_conn_lookup";
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_NOTIF_PREP:
        return "agent_post_phase_notif_prep";
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_MD_VALIDATE:
        return "agent_post_phase_md_validate";
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_FI_MORE_PREPASS:
        return "agent_post_phase_fi_more_prepass";
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_SUBMIT_LOOP:
        return "agent_post_phase_submit_loop";
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_NOTIF_SEND:
        return "agent_post_phase_notif_send";
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_PROGRESS_TAIL:
        return "agent_post_phase_progress_tail";
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_RAIL_SELECT:
        return "agent_post_accum_rail_select";
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_REQ_ALLOC:
        return "agent_post_accum_req_alloc";
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_FI_WRITEMSG:
        return "agent_post_accum_fi_writemsg";
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_EAGAIN_DRAIN:
        return "agent_post_accum_eagain_drain";
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_CUDA_CTX:
        return "agent_post_accum_cuda_ctx";
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_CALIBRATION:
        return "agent_post_phase_calibration";
    case nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_ATTEMPTS:
        return "agent_post_eagain_attempts";
    case nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_MAX_ATTEMPTS:
        return "agent_post_eagain_max_attempts";
    case nixl_telemetry_event_type_t::AGENT_POST_SUBMITTED_REQUESTS:
        return "agent_post_submitted_requests";
    case nixl_telemetry_event_type_t::AGENT_POST_RAILS_TOUCHED:
        return "agent_post_rails_touched";
    case nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED:
        return "agent_telemetry_events_dropped";
    }
    return "unknown_event";
}

[[nodiscard]] constexpr const char *
telemetryErrorStatusLabel(const nixl_telemetry_event_type_t type) noexcept {
    switch (type) {
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_POSTED:
        return "not_posted";
    case nixl_telemetry_event_type_t::AGENT_ERR_INVALID_PARAM:
        return "invalid_param";
    case nixl_telemetry_event_type_t::AGENT_ERR_BACKEND:
        return "backend";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_FOUND:
        return "not_found";
    case nixl_telemetry_event_type_t::AGENT_ERR_MISMATCH:
        return "mismatch";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_ALLOWED:
        return "not_allowed";
    case nixl_telemetry_event_type_t::AGENT_ERR_REPOST_ACTIVE:
        return "repost_active";
    case nixl_telemetry_event_type_t::AGENT_ERR_UNKNOWN:
        return "unknown";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_SUPPORTED:
        return "not_supported";
    case nixl_telemetry_event_type_t::AGENT_ERR_REMOTE_DISCONNECT:
        return "remote_disconnect";
    case nixl_telemetry_event_type_t::AGENT_ERR_CANCELED:
        return "canceled";
    case nixl_telemetry_event_type_t::AGENT_ERR_NO_TELEMETRY:
        return "no_telemetry";
    case nixl_telemetry_event_type_t::AGENT_TX_BYTES:
    case nixl_telemetry_event_type_t::AGENT_RX_BYTES:
    case nixl_telemetry_event_type_t::AGENT_TX_REQUESTS_NUM:
    case nixl_telemetry_event_type_t::AGENT_RX_REQUESTS_NUM:
    case nixl_telemetry_event_type_t::AGENT_MEMORY_REGISTERED:
    case nixl_telemetry_event_type_t::AGENT_MEMORY_DEREGISTERED:
    case nixl_telemetry_event_type_t::AGENT_XFER_TIME:
    case nixl_telemetry_event_type_t::AGENT_XFER_POST_TIME:
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_CONN_LOOKUP:
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_NOTIF_PREP:
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_MD_VALIDATE:
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_FI_MORE_PREPASS:
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_SUBMIT_LOOP:
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_NOTIF_SEND:
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_PROGRESS_TAIL:
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_RAIL_SELECT:
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_REQ_ALLOC:
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_FI_WRITEMSG:
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_EAGAIN_DRAIN:
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_CUDA_CTX:
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_CALIBRATION:
    case nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_ATTEMPTS:
    case nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_MAX_ATTEMPTS:
    case nixl_telemetry_event_type_t::AGENT_POST_SUBMITTED_REQUESTS:
    case nixl_telemetry_event_type_t::AGENT_POST_RAILS_TOUCHED:
    case nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED:
        return nullptr;
    }
    return nullptr;
}

/**
 * @brief Exporter-side Prometheus series descriptor for a telemetry event.
 *
 * The native Prometheus, multi-process Prometheus and DOCA/CollectX exporters
 * derive their series from this single mapping, so they emit the same metric
 * definitions. A null @c counterName, @c gaugeName, or @c histogramName means the
 * event has no cumulative counter, no last-operation gauge, or no distribution
 * histogram, respectively. Error events (@c AGENT_ERR_*) and any unmapped value
 * return an all-null descriptor.
 *
 * @param type Telemetry event type.
 * @return Counter/gauge series names and HELP strings for @p type.
 */
[[nodiscard]] constexpr nixlTelemetryMetricDescriptor
telemetryMetricDescriptor(const nixl_telemetry_event_type_t type) noexcept {
    switch (type) {
    case nixl_telemetry_event_type_t::AGENT_TX_BYTES:
        return {"agent_tx_bytes_total",
                "Number of bytes sent by the agent",
                "agent_tx_last_bytes",
                "Bytes sent by the last request",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_RX_BYTES:
        return {"agent_rx_bytes_total",
                "Number of bytes received by the agent",
                "agent_rx_last_bytes",
                "Bytes received by the last request",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_TX_REQUESTS_NUM:
        return {"agent_tx_requests_num_total",
                "Number of requests sent by the agent",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_RX_REQUESTS_NUM:
        return {"agent_rx_requests_num_total",
                "Number of requests received by the agent",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_MEMORY_REGISTERED:
        return {"agent_memory_registered_total",
                "Cumulative memory registered",
                "agent_memory_registered_last_bytes",
                "Memory registered by the last operation",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_MEMORY_DEREGISTERED:
        return {"agent_memory_deregistered_total",
                "Cumulative memory deregistered",
                "agent_memory_deregistered_last_bytes",
                "Memory deregistered by the last operation",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_XFER_TIME:
        return {"agent_xfer_time_total",
                "Cumulative sum of transfer time from start to completion",
                "agent_xfer_time",
                "Transfer time of the last request",
                "agent_xfer_time_us",
                "Distribution of transfer time from start to completion, in microseconds"};
    case nixl_telemetry_event_type_t::AGENT_XFER_POST_TIME:
        return {"agent_xfer_post_time_total",
                "Cumulative sum of time from start to posting to the back-end",
                "agent_xfer_post_time",
                "Post time of the last request",
                "agent_xfer_post_time_us",
                "Distribution of time from start to posting to the back-end, in microseconds"};
    // postXfer-path profiling. Values are nanoseconds, so every row scales by
    // 0.001 to reach the microsecond histogram buckets.
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_CONN_LOOKUP:
        return {"agent_post_phase_conn_lookup_ns_total",
                "Cumulative time in the postXfer connection lookup and state check",
                "agent_post_phase_conn_lookup_ns",
                "Connection lookup and state check of the last posted transfer",
                "agent_post_phase_conn_lookup_us",
                "Distribution of postXfer connection lookup time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_NOTIF_PREP:
        return {"agent_post_phase_notif_prep_ns_total",
                "Cumulative time preparing/fragmenting the postXfer notification",
                "agent_post_phase_notif_prep_ns",
                "Notification preparation of the last posted transfer",
                "agent_post_phase_notif_prep_us",
                "Distribution of postXfer notification preparation time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_MD_VALIDATE:
        return {"agent_post_phase_md_validate_ns_total",
                "Cumulative time in the postXfer metadata validation pre-pass",
                "agent_post_phase_md_validate_ns",
                "Metadata validation pre-pass of the last posted transfer",
                "agent_post_phase_md_validate_us",
                "Distribution of postXfer metadata validation time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_FI_MORE_PREPASS:
        return {"agent_post_phase_fi_more_prepass_ns_total",
                "Cumulative time in the FI_MORE last-descriptor-per-rail pre-pass",
                "agent_post_phase_fi_more_prepass_ns",
                "FI_MORE pre-pass of the last posted transfer",
                "agent_post_phase_fi_more_prepass_us",
                "Distribution of FI_MORE pre-pass time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_SUBMIT_LOOP:
        return {"agent_post_phase_submit_loop_ns_total",
                "Cumulative time in the postXfer per-descriptor submit loop",
                "agent_post_phase_submit_loop_ns",
                "Descriptor submit loop of the last posted transfer",
                "agent_post_phase_submit_loop_us",
                "Distribution of postXfer submit loop time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_NOTIF_SEND:
        return {"agent_post_phase_notif_send_ns_total",
                "Cumulative time sending the postXfer notification",
                "agent_post_phase_notif_send_ns",
                "Notification send of the last posted transfer",
                "agent_post_phase_notif_send_us",
                "Distribution of postXfer notification send time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_PROGRESS_TAIL:
        return {"agent_post_phase_progress_tail_ns_total",
                "Cumulative time in the inline completion-queue progress tail of postXfer",
                "agent_post_phase_progress_tail_ns",
                "Inline progress tail of the last posted transfer",
                "agent_post_phase_progress_tail_us",
                "Distribution of the postXfer inline progress tail, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_RAIL_SELECT:
        return {"agent_post_accum_rail_select_ns_total",
                "Cumulative rail-selection time summed over posted descriptors",
                "agent_post_accum_rail_select_ns",
                "Rail selection summed over the last posted transfer",
                "agent_post_accum_rail_select_us",
                "Distribution of per-transfer summed rail-selection time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_REQ_ALLOC:
        return {"agent_post_accum_req_alloc_ns_total",
                "Cumulative request-pool allocation time summed over posted descriptors",
                "agent_post_accum_req_alloc_ns",
                "Request-pool allocation summed over the last posted transfer",
                "agent_post_accum_req_alloc_us",
                "Distribution of per-transfer summed request allocation time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_FI_WRITEMSG:
        return {"agent_post_accum_fi_writemsg_ns_total",
                "Cumulative first-attempt fi_writemsg time summed over posted descriptors",
                "agent_post_accum_fi_writemsg_ns",
                "First-attempt fi_writemsg summed over the last posted transfer",
                "agent_post_accum_fi_writemsg_us",
                "Distribution of per-transfer summed fi_writemsg time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_EAGAIN_DRAIN:
        return {"agent_post_accum_eagain_drain_ns_total",
                "Cumulative time draining completions inline after -FI_EAGAIN",
                "agent_post_accum_eagain_drain_ns",
                "Inline EAGAIN drain summed over the last posted transfer",
                "agent_post_accum_eagain_drain_us",
                "Distribution of per-transfer summed EAGAIN drain time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_ACCUM_CUDA_CTX:
        return {"agent_post_accum_cuda_ctx_ns_total",
                "Cumulative CUDA context/device handling time on the post path",
                "agent_post_accum_cuda_ctx_ns",
                "CUDA context handling summed over the last posted transfer",
                "agent_post_accum_cuda_ctx_us",
                "Distribution of per-transfer summed CUDA context time, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_PHASE_CALIBRATION:
        return {"agent_post_phase_calibration_ns_total",
                "Cumulative measured cost of the profiling harness over a zero-length region",
                "agent_post_phase_calibration_ns",
                "Profiling harness cost measured during the last posted transfer",
                "agent_post_phase_calibration_us",
                "Distribution of the profiling harness cost, in microseconds",
                0.001};
    case nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_ATTEMPTS:
        return {"agent_post_eagain_attempts_total",
                "Cumulative -FI_EAGAIN retry attempts on the post path",
                "agent_post_eagain_attempts",
                "-FI_EAGAIN retry attempts during the last posted transfer",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_POST_EAGAIN_MAX_ATTEMPTS:
        // The per-post maximum is a gauge by nature; the counter accumulates
        // those maxima so that rate() over the transfer rate gives their mean.
        return {"agent_post_eagain_max_attempts_total",
                "Cumulative sum of the per-transfer maximum -FI_EAGAIN retry count",
                "agent_post_eagain_max_attempts",
                "Highest -FI_EAGAIN retry count for any single descriptor of the last transfer",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_POST_SUBMITTED_REQUESTS:
        return {"agent_post_submitted_requests_total",
                "Cumulative descriptors submitted to the provider by postXfer",
                "agent_post_submitted_requests",
                "Descriptors submitted by the last posted transfer",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_POST_RAILS_TOUCHED:
        return {"agent_post_rails_touched_total",
                "Cumulative count of rails posted to, summed over transfers",
                "agent_post_rails_touched",
                "Distinct rails posted to by the last posted transfer",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED:
        return {"agent_telemetry_events_dropped_total",
                "Cumulative telemetry events dropped at the producer-side staging queue",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_POSTED:
    case nixl_telemetry_event_type_t::AGENT_ERR_INVALID_PARAM:
    case nixl_telemetry_event_type_t::AGENT_ERR_BACKEND:
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_FOUND:
    case nixl_telemetry_event_type_t::AGENT_ERR_MISMATCH:
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_ALLOWED:
    case nixl_telemetry_event_type_t::AGENT_ERR_REPOST_ACTIVE:
    case nixl_telemetry_event_type_t::AGENT_ERR_UNKNOWN:
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_SUPPORTED:
    case nixl_telemetry_event_type_t::AGENT_ERR_REMOTE_DISCONNECT:
    case nixl_telemetry_event_type_t::AGENT_ERR_CANCELED:
    case nixl_telemetry_event_type_t::AGENT_ERR_NO_TELEMETRY:
        break;
    }
    return {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
}
} // namespace nixlEnumStrings

/**
 * @struct nixlTelemetryEvent
 * @brief A structure to hold individual telemetry event data for cyclic buffer storage
 */
struct nixlTelemetryEvent {
    nixl_telemetry_event_type_t eventType_; // Detailed event type/identifier
    uint64_t value_; // Numeric value associated with the event

    nixlTelemetryEvent() noexcept = default;

    nixlTelemetryEvent(nixl_telemetry_event_type_t event_type, uint64_t value) noexcept
        : eventType_(event_type),
          value_(value) {}
};

#endif
