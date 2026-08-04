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

/*
 * Unit test for the nixl::trace instrumentation in nixlLibfabricRail.
 *
 * test/gtest/unit/tracing covers the facade itself (fan-out, correlation, inert
 * paths). This test covers what the libfabric post paths do with it: that each
 * post emits one span with the expected name, Kind, attributes and correlation
 * id, and that a rail built without a tracer emits nothing.
 *
 * The rail is built against the shared libfabric stubs (no hardware). The stubs
 * only cover what rail construction needs, so this file additionally installs
 * the send/rma ops the post paths dispatch through -- see installPostOps().
 */

#include "libfabric/libfabric_rail.h"
#include "libfabric/libfabric_common.h"
#include "common/nixl_log.h"
#include "tracing/trace.h"
#include "libfabric_mock_stubs.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// --- Unconditional __wrap_* functions ---

extern "C" int
__wrap_numa_max_node() {
    return 1;
}

extern "C" int
__wrap_numa_num_configured_nodes() {
    return 2;
}

extern "C" int
__wrap_fi_getinfo(uint32_t /*version*/,
                  const char * /*node*/,
                  const char * /*service*/,
                  uint64_t /*flags*/,
                  const struct fi_info * /*hints*/,
                  struct fi_info **info) {
    // One fake EFA device; a directly-constructed rail uses the first entry.
    fi_info *fi = malloc_zero<fi_info>();

    fi->domain_attr = malloc_zero<fi_domain_attr>();
    fi->domain_attr->name = strdup("efa_0");

    fi->fabric_attr = malloc_zero<fi_fabric_attr>();
    fi->fabric_attr->prov_name = strdup("efa");
    fi->fabric_attr->name = strdup("efa");

    fi->ep_attr = malloc_zero<fi_ep_attr>();
    fi->ep_attr->type = FI_EP_RDM;

    fi->nic = malloc_zero<fid_nic>();
    fi->nic->bus_attr = malloc_zero<fi_bus_attr>();
    fi->nic->bus_attr->bus_type = FI_BUS_PCI;
    fi->nic->link_attr = malloc_zero<fi_link_attr>();
    fi->nic->link_attr->speed = 100ull * NIXL_LIBFABRIC_GIGA;

    *info = fi;
    return 0;
}

extern "C" int
__wrap_fi_fabric(struct fi_fabric_attr * /*attr*/, struct fid_fabric **fabric, void * /*context*/) {
    *fabric = mock_fabric_create();
    return 0;
}

// --- Post-path libfabric ops ---
//
// libfabric_mock_stubs.h stubs only what rail construction touches (recvmsg for
// the pre-posted recv pool). postSend/postWrite/postRead dispatch through
// ep->msg->senddata, ep->rma->writemsg and ep->rma->read respectively; without
// these, fi_senddata() and friends call through a null pointer. Each accepts the
// operation immediately so no post retries, and therefore no CQ progress (the
// stub CQ has no ops), is triggered.

namespace {

std::size_t g_senddata_calls = 0;
std::size_t g_writemsg_calls = 0;
std::size_t g_read_calls = 0;

} // namespace

static ssize_t
fi_ep_senddata_stub(struct fid_ep * /*ep*/,
                    const void * /*buf*/,
                    size_t /*len*/,
                    void * /*desc*/,
                    uint64_t /*data*/,
                    fi_addr_t /*dest_addr*/,
                    void * /*context*/) {
    ++g_senddata_calls;
    return 0;
}

static ssize_t
fi_ep_rma_writemsg_stub(struct fid_ep * /*ep*/,
                        const struct fi_msg_rma * /*msg*/,
                        uint64_t /*flags*/) {
    ++g_writemsg_calls;
    return 0;
}

static ssize_t
fi_ep_rma_read_stub(struct fid_ep * /*ep*/,
                    void * /*buf*/,
                    size_t /*len*/,
                    void * /*desc*/,
                    fi_addr_t /*src_addr*/,
                    uint64_t /*addr*/,
                    uint64_t /*key*/,
                    void * /*context*/) {
    ++g_read_calls;
    return 0;
}

static fi_ops_rma fi_ep_rma_ops_stub{
    .read = fi_ep_rma_read_stub,
    .writemsg = fi_ep_rma_writemsg_stub,
};

// Extend the shared endpoint op tables in place with the transmit-side ops. Call
// after the rail is constructed; the rail's `endpoint` member is public.
static void
installPostOps(nixlLibfabricRail &rail) {
    fi_ep_msg_ops_stub.senddata = fi_ep_senddata_stub;
    rail.endpoint->rma = &fi_ep_rma_ops_stub;
}

namespace {

// --- Recording trace backend ---

// One span or mark as the backend saw it, with the attributes attached to it.
struct RecordedEvent {
    std::string name;
    nixl::trace::Kind kind = nixl::trace::Kind::Generic;
    bool isSpan = false; // false => mark
    // Correlation id active when the event was created, or 0 if none. This is
    // what NVTX folds into the event payload.
    std::uint64_t correlationId = 0;
    bool ended = false;
    std::vector<std::pair<std::string, std::int64_t>> intAttrs;
    std::vector<std::string> strAttrKeys;

    [[nodiscard]] bool
    hasAttr(std::string_view key) const {
        for (const auto &kv : intAttrs) {
            if (kv.first == key) return true;
        }
        for (const auto &k : strAttrKeys) {
            if (k == key) return true;
        }
        return false;
    }

    // Returns INT64_MIN for a missing key, a value no call site produces, so a
    // missing attribute fails an equality assert instead of reading as 0.
    [[nodiscard]] std::int64_t
    intAttr(std::string_view key) const {
        for (const auto &kv : intAttrs) {
            if (kv.first == key) return kv.second;
        }
        return INT64_MIN;
    }
};

// Spans write their attributes back into the event they belong to, so assertions
// can check name/kind/attributes together rather than against flat lists.
struct Recorder {
    std::vector<RecordedEvent> events;
    std::vector<std::uint64_t> correlationStack;

    [[nodiscard]] std::uint64_t
    currentCorrelation() const {
        return correlationStack.empty() ? 0 : correlationStack.back();
    }

    [[nodiscard]] const RecordedEvent *
    find(std::string_view name) const {
        for (const auto &e : events) {
            if (e.name == name) return &e;
        }
        return nullptr;
    }
};

class RecordingSpan final : public nixl::trace::SpanBackend {
public:
    RecordingSpan(Recorder *rec, std::size_t index) : rec_(rec), index_(index) {}

    ~RecordingSpan() override {
        rec_->events[index_].ended = true;
    }

    void
    addAttribute(std::string_view key, std::string_view) override {
        rec_->events[index_].strAttrKeys.emplace_back(key);
    }

    void
    addAttribute(std::string_view key, std::int64_t value) override {
        rec_->events[index_].intAttrs.emplace_back(std::string(key), value);
    }

    void
    addAttribute(std::string_view, double) override {}

    void
    addCtrlDep(nixl::trace::SpanId) override {}

    void
    addDataDep(nixl::trace::SpanId) override {}

    [[nodiscard]] nixl::trace::SpanId
    id() const noexcept override {
        return {};
    }

private:
    Recorder *rec_;
    std::size_t index_;
};

class RecordingBackend final : public nixl::trace::TraceBackend {
public:
    explicit RecordingBackend(Recorder *rec) : rec_(rec) {}

    [[nodiscard]] std::unique_ptr<nixl::trace::SpanBackend>
    beginSpan(std::string_view name, nixl::trace::Kind kind) override {
        RecordedEvent ev;
        ev.name = std::string(name);
        ev.kind = kind;
        ev.isSpan = true;
        ev.correlationId = rec_->currentCorrelation();
        rec_->events.push_back(std::move(ev));
        return std::make_unique<RecordingSpan>(rec_, rec_->events.size() - 1);
    }

    void
    mark(std::string_view name, nixl::trace::Kind kind) override {
        RecordedEvent ev;
        ev.name = std::string(name);
        ev.kind = kind;
        ev.isSpan = false;
        ev.correlationId = rec_->currentCorrelation();
        ev.ended = true; // marks are instantaneous
        rec_->events.push_back(std::move(ev));
    }

    void
    pushCorrelationId(std::uint64_t id) override {
        rec_->correlationStack.push_back(id);
    }

    void
    popCorrelationId() override {
        if (!rec_->correlationStack.empty()) rec_->correlationStack.pop_back();
    }

    [[nodiscard]] std::string_view
    name() const noexcept override {
        return "recording";
    }

private:
    Recorder *rec_;
};

[[nodiscard]] std::unique_ptr<nixl::trace::Tracer>
makeRecordingTracer(Recorder &rec) {
    std::vector<std::unique_ptr<nixl::trace::TraceBackend>> backends;
    backends.push_back(std::make_unique<RecordingBackend>(&rec));
    return std::make_unique<nixl::trace::Tracer>(std::move(backends));
}

// --- Test helpers ---

#define TEST_ASSERT(cond, msg)                                                           \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "FAIL: " << (msg) << " [" << __FILE__ << ":" << __LINE__ << "]" \
                      << std::endl;                                                      \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

constexpr uint16_t kRailId = 0;
constexpr uint32_t kXferId = 0x2A;
constexpr int kDeviceId = 3;
constexpr size_t kTransferLen = 4096;
constexpr size_t kControlMsgLen = 128;

// Destination address, remote address and key: arbitrary, since the stubbed
// msg/rma ops ignore them.
constexpr fi_addr_t kDestAddr = 1;
constexpr uint64_t kRemoteAddr = 0xDEAD0000ull;
constexpr uint64_t kRemoteKey = 0x99ull;

// --- Tests ---

// A rail built without a tracer must run the same post paths and emit nothing.
// This is the configuration of every agent that did not request a backend, and
// the only one available when tracing is compiled out.
static int
testNullTracerEmitsNothing() {
    NIXL_INFO << "  testNullTracerEmitsNothing";

    Recorder rec;
    // Default tracer argument is nullptr.
    nixlLibfabricRail rail("efa_0", "efa", kRailId);
    TEST_ASSERT(rail.isProperlyInitialized(), "rail initialized without a tracer");
    installPostOps(rail);

    nixlLibfabricReq *req = rail.allocateDataRequest(nixlLibfabricReq::WRITE, kXferId);
    TEST_ASSERT(req != nullptr, "data request allocated");

    std::vector<char> buf(kTransferLen, 0);
    const std::size_t writes_before = g_writemsg_calls;
    nixl_status_t status = rail.postWrite(buf.data(),
                                          kTransferLen,
                                          /*local_desc=*/nullptr,
                                          /*immediate_data=*/0,
                                          kDestAddr,
                                          kRemoteAddr,
                                          kRemoteKey,
                                          req);
    TEST_ASSERT(status == NIXL_SUCCESS, "postWrite succeeds with a null tracer");
    TEST_ASSERT(g_writemsg_calls == writes_before + 1, "the post still reached libfabric");
    TEST_ASSERT(rec.events.empty(), "no events recorded without a tracer");

    rail.releaseRequest(req);
    return 0;
}

// device_id is pool state reused across requests, so release() must reset it to
// the -1 "unknown" sentinel; otherwise a recycled request would report the
// previous owner's GPU in its span.
static int
testReleaseResetsDeviceId() {
    NIXL_INFO << "  testReleaseResetsDeviceId";

    nixlLibfabricRail rail("efa_0", "efa", kRailId);

    nixlLibfabricReq *req = rail.allocateDataRequest(nixlLibfabricReq::WRITE, kXferId);
    TEST_ASSERT(req != nullptr, "data request allocated");
    TEST_ASSERT(req->device_id == -1, "freshly allocated request has no device");

    req->device_id = kDeviceId;
    rail.releaseRequest(req);
    // The request stays in the pool after release, so reading it back is valid.
    TEST_ASSERT(req->device_id == -1, "release() reset device_id to the unknown sentinel");

    return 0;
}

#if defined(NIXL_TRACE_ENABLED)

// A WRITE post emits exactly one span, kinded and attributed per the call site
// and tagged with the request's xfer_id.
static int
testPostWriteEmitsSpan() {
    NIXL_INFO << "  testPostWriteEmitsSpan";

    Recorder rec;
    auto tracer = makeRecordingTracer(rec);
    nixlLibfabricRail rail("efa_0", "efa", kRailId, tracer.get());
    TEST_ASSERT(rail.isProperlyInitialized(), "rail initialized against stubs");
    installPostOps(rail);

    nixlLibfabricReq *req = rail.allocateDataRequest(nixlLibfabricReq::WRITE, kXferId);
    TEST_ASSERT(req != nullptr, "data request allocated");
    req->device_id = kDeviceId;

    std::vector<char> buf(kTransferLen, 0);
    rec.events.clear();
    nixl_status_t status = rail.postWrite(buf.data(),
                                          kTransferLen,
                                          /*local_desc=*/nullptr,
                                          /*immediate_data=*/0,
                                          kDestAddr,
                                          kRemoteAddr,
                                          kRemoteKey,
                                          req);
    TEST_ASSERT(status == NIXL_SUCCESS, "postWrite succeeded against stubs");

    TEST_ASSERT(rec.events.size() == 1, "postWrite emitted exactly one event");
    const RecordedEvent *ev = rec.find("nixl::libfabric.post_write");
    TEST_ASSERT(ev != nullptr, "post_write span present");
    TEST_ASSERT(ev->isSpan, "post_write is a span, not a mark");
    TEST_ASSERT(ev->ended, "span ended when postWrite returned (RAII)");

    // An RDMA write is a network send, not a local memory store. This has to
    // agree with the enclosing transfer span and with post_send.
    TEST_ASSERT(ev->kind == nixl::trace::Kind::CommSend, "post_write kind is CommSend");

    TEST_ASSERT(ev->intAttr("device_id") == kDeviceId, "device_id attr");
    TEST_ASSERT(ev->intAttr("rail_id") == kRailId, "rail_id attr");
    TEST_ASSERT(ev->intAttr("length") == static_cast<std::int64_t>(kTransferLen), "length attr");
    TEST_ASSERT(ev->intAttr("xfer_id") == kXferId, "xfer_id attr");
    // Recorded on the success path only, where the attempt count is known.
    TEST_ASSERT(ev->hasAttr("retries"), "retries attr recorded on success");
    TEST_ASSERT(ev->intAttr("retries") == 0, "no retries when the op is accepted immediately");

    // Tagged with the xfer_id so it links to the completion, which is marked on
    // the progress thread.
    TEST_ASSERT(ev->correlationId == kXferId, "span correlated on xfer_id");
    // The call site's CorrelationScope must have popped by now.
    TEST_ASSERT(rec.correlationStack.empty(), "correlation stack balanced after postWrite");

    rail.releaseRequest(req);
    return 0;
}

// READ mirrors WRITE: inbound data, so CommRecv rather than CommSend.
static int
testPostReadEmitsSpan() {
    NIXL_INFO << "  testPostReadEmitsSpan";

    Recorder rec;
    auto tracer = makeRecordingTracer(rec);
    nixlLibfabricRail rail("efa_0", "efa", kRailId, tracer.get());
    installPostOps(rail);

    nixlLibfabricReq *req = rail.allocateDataRequest(nixlLibfabricReq::READ, kXferId);
    TEST_ASSERT(req != nullptr, "data request allocated");
    req->device_id = kDeviceId;

    std::vector<char> buf(kTransferLen, 0);
    rec.events.clear();
    nixl_status_t status = rail.postRead(buf.data(),
                                         kTransferLen,
                                         /*local_desc=*/nullptr,
                                         kDestAddr,
                                         kRemoteAddr,
                                         kRemoteKey,
                                         req);
    TEST_ASSERT(status == NIXL_SUCCESS, "postRead succeeded against stubs");

    TEST_ASSERT(rec.events.size() == 1, "postRead emitted exactly one event");
    const RecordedEvent *ev = rec.find("nixl::libfabric.post_read");
    TEST_ASSERT(ev != nullptr, "post_read span present");
    TEST_ASSERT(ev->ended, "span ended when postRead returned (RAII)");
    TEST_ASSERT(ev->kind == nixl::trace::Kind::CommRecv, "post_read kind is CommRecv");
    TEST_ASSERT(ev->intAttr("device_id") == kDeviceId, "device_id attr");
    TEST_ASSERT(ev->intAttr("rail_id") == kRailId, "rail_id attr");
    TEST_ASSERT(ev->intAttr("length") == static_cast<std::int64_t>(kTransferLen), "length attr");
    TEST_ASSERT(ev->intAttr("xfer_id") == kXferId, "xfer_id attr");
    TEST_ASSERT(ev->intAttr("retries") == 0, "retries attr");
    TEST_ASSERT(ev->correlationId == kXferId, "span correlated on xfer_id");
    TEST_ASSERT(rec.correlationStack.empty(), "correlation stack balanced after postRead");

    rail.releaseRequest(req);
    return 0;
}

// A control-message send has no request-level xfer_id to read, so the call site
// decodes it from the immediate data. It also records no device_id: control
// buffers are host memory with no owning GPU, so the attribute would be a
// constant -1 on every span.
static int
testPostSendCorrelatesOnImmDataXferId() {
    NIXL_INFO << "  testPostSendCorrelatesOnImmDataXferId";

    Recorder rec;
    auto tracer = makeRecordingTracer(rec);
    nixlLibfabricRail rail("efa_0", "efa", kRailId, tracer.get());
    installPostOps(rail);

    nixlLibfabricReq *req = rail.allocateControlRequest(kControlMsgLen, kXferId);
    TEST_ASSERT(req != nullptr, "control request allocated");

    // Deliberately different from req->xfer_id, so the assertions below can tell
    // which of the two the span actually used. Must fit NIXL_XFER_ID_MASK.
    const uint64_t imm_xfer_id = 0x1234;
    const uint64_t imm_data =
        NIXL_MAKE_IMM_DATA(NIXL_LIBFABRIC_MSG_NOTIFICTION, /*agent_idx=*/1, imm_xfer_id, 0);

    rec.events.clear();
    nixl_status_t status = rail.postSend(imm_data, kDestAddr, req);
    TEST_ASSERT(status == NIXL_SUCCESS, "postSend succeeded against stubs");

    TEST_ASSERT(rec.events.size() == 1, "postSend emitted exactly one event");
    const RecordedEvent *ev = rec.find("nixl::libfabric.post_send");
    TEST_ASSERT(ev != nullptr, "post_send span present");
    TEST_ASSERT(ev->ended, "span ended when postSend returned (RAII)");
    TEST_ASSERT(ev->kind == nixl::trace::Kind::CommSend, "post_send kind is CommSend");
    TEST_ASSERT(ev->intAttr("rail_id") == kRailId, "rail_id attr");
    TEST_ASSERT(ev->intAttr("length") == static_cast<std::int64_t>(kControlMsgLen), "length attr");
    TEST_ASSERT(ev->intAttr("xfer_id") == static_cast<std::int64_t>(imm_xfer_id),
                "xfer_id decoded from the immediate data, not taken from req");
    TEST_ASSERT(ev->correlationId == imm_xfer_id, "correlated on the imm-data xfer_id");
    TEST_ASSERT(!ev->hasAttr("device_id"), "no device_id attr on the control path");
    TEST_ASSERT(rec.correlationStack.empty(), "correlation stack balanced after postSend");

    rail.releaseRequest(req);
    return 0;
}

// Every emitted span name must be in the NVTX backend's kRegisteredSpanNames
// table, or NVTX falls back to an unregistered string per event. That table is
// in the nvtx plugin, which these tests do not link, so assert the property this
// test can see: the names are the exact literals the registry is kept in sync
// with. Update src/plugins/tracing/nvtx/nvtx_events.cpp alongside any change.
static int
testEmittedSpanNamesAreStable() {
    NIXL_INFO << "  testEmittedSpanNamesAreStable";

    Recorder rec;
    auto tracer = makeRecordingTracer(rec);
    nixlLibfabricRail rail("efa_0", "efa", kRailId, tracer.get());
    installPostOps(rail);

    std::vector<char> buf(kTransferLen, 0);

    nixlLibfabricReq *wreq = rail.allocateDataRequest(nixlLibfabricReq::WRITE, kXferId);
    TEST_ASSERT(wreq != nullptr, "write request allocated");
    TEST_ASSERT(rail.postWrite(buf.data(),
                               kTransferLen,
                               nullptr,
                               0,
                               kDestAddr,
                               kRemoteAddr,
                               kRemoteKey,
                               wreq) == NIXL_SUCCESS,
                "postWrite succeeded");
    rail.releaseRequest(wreq);

    nixlLibfabricReq *rreq = rail.allocateDataRequest(nixlLibfabricReq::READ, kXferId);
    TEST_ASSERT(rreq != nullptr, "read request allocated");
    TEST_ASSERT(
        rail.postRead(buf.data(), kTransferLen, nullptr, kDestAddr, kRemoteAddr, kRemoteKey, rreq) ==
            NIXL_SUCCESS,
        "postRead succeeded");
    rail.releaseRequest(rreq);

    nixlLibfabricReq *sreq = rail.allocateControlRequest(kControlMsgLen, kXferId);
    TEST_ASSERT(sreq != nullptr, "control request allocated");
    TEST_ASSERT(rail.postSend(NIXL_MAKE_IMM_DATA(NIXL_LIBFABRIC_MSG_NOTIFICTION, 1, kXferId, 0),
                              kDestAddr,
                              sreq) == NIXL_SUCCESS,
                "postSend succeeded");
    rail.releaseRequest(sreq);

    for (const auto *name : {"nixl::libfabric.post_write",
                             "nixl::libfabric.post_read",
                             "nixl::libfabric.post_send"}) {
        TEST_ASSERT(rec.find(name) != nullptr, name);
    }

    // Every name is namespaced, so a trace is filterable by backend.
    for (const auto &ev : rec.events) {
        TEST_ASSERT(ev.name.rfind("nixl::libfabric.", 0) == 0,
                    "span name carries the nixl::libfabric prefix");
    }

    return 0;
}

#endif // NIXL_TRACE_ENABLED

} // namespace

int
main() {
    NIXL_INFO << "=== Rail Tracing Test ===";
    NIXL_INFO << "Using mock stubs (__wrap_fi_getinfo, __wrap_fi_fabric, etc.)";

    int res;
    if ((res = testNullTracerEmitsNothing()) != 0) return res;
    if ((res = testReleaseResetsDeviceId()) != 0) return res;

#if defined(NIXL_TRACE_ENABLED)
    if ((res = testPostWriteEmitsSpan()) != 0) return res;
    if ((res = testPostReadEmitsSpan()) != 0) return res;
    if ((res = testPostSendCorrelatesOnImmDataXferId()) != 0) return res;
    if ((res = testEmittedSpanNamesAreStable()) != 0) return res;
#else
    NIXL_INFO << "  (span-emission tests skipped: built without NIXL_TRACE_ENABLED)";
#endif

    NIXL_INFO << "=== All rail tracing tests PASSED ===";
    return 0;
}
