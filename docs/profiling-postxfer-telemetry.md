# Profiling the `postXfer` Path with Telemetry

How to locate where time is spent between `nixlAgent::postXferReq()` and
`fi_writemsg()` in the libfabric backend, using the telemetry subsystem as the
measurement vehicle.

**Status:** the harness described here is implemented. §1–§5 are the reasoning
behind its shape, §6 records what was built and how the plan's open questions
were settled, and §7 is the run protocol — start there if you only want to take
a measurement. The single knob is `NIXL_POST_PROFILE` (§6.5); with it unset,
nothing in this document costs anything at runtime.

**Context:** NCCLOFI-1865 (gpt-oss perf headroom analysis). `nixlbench` reports
post latency growing with batch size on the **progress-thread-off (PT OFF)**
path, so the whole path of interest runs on a single thread.

**Scope:** PT OFF, `NIXL_WRITE`, single posting thread. The PT ON path defers
posts through the ring buffer and is a different problem.

File/line references were verified against `origin/main` at `fa01dd3a` and
refreshed after the harness landed. Function names are the durable anchors; line
numbers will drift.

---

## 1. The path under measurement

```
nixlAgent::postXferReq()                    src/core/nixl_agent.cpp:1084
  └─ nixlLibfabricEngine::postXfer()        src/plugins/libfabric/libfabric_backend.cpp:1448
       └─ postXferDescriptors()             src/plugins/libfabric/libfabric_backend.cpp:1286
            └─ railManager::prepareAndSubmitTransfer()
                                            src/utils/libfabric/libfabric_rail_manager.cpp:377
                 └─ nixlLibfabricRail::postWrite()
                                            src/utils/libfabric/libfabric_rail.cpp:1349
                      └─ fi_writemsg()      src/utils/libfabric/libfabric_rail.cpp:1405
```

---

## 2. Why telemetry rather than NVTX tracing

Both subsystems exist. They are not interchangeable for this job.

### Per-event cost, read from the code

**Telemetry — O(1) per transfer:**

- Two `steady_clock::now()` calls: `timer.restart()` (`nixl_agent.cpp:1110`) and
  `timer.elapsed()` (`nixl_agent.cpp:83`).
- `addXferStats()` → `nixlTelemetryStagingQueue::tryPushBatch()`
  (`telemetry_staging_queue.cpp:36`): one uncontended mutex plus a
  `vector::insert` into pre-reserved storage.
- **Zero heap allocations.** Export runs off-thread on an asio pool.
- Deactivated metrics are dropped before the staging queue, so they cost
  nothing (`docs/telemetry.md:155`).

**NVTX tracing — per span:**

- `Tracer::beginSpan()` (`tracer.cpp:85`) builds a
  `std::vector<std::unique_ptr<SpanBackend>>` → **1 heap allocation**.
- `NvtxTraceBackend::beginSpan()` (`nvtx_trace_backend.cpp:69`)
  does `make_unique<NvtxSpan>` → **1 heap allocation**.
- `eventForName()` → `lookupRegistered()`: linear scan with string compares over
  the registered-name vector, on every span.
- `nvtxDomainRangePushEx()`, then `nvtxDomainRangePop()` in `~NvtxSpan`, then two
  frees.
- Every `NIXL_TRACE_ATTR` adds a `std::list::emplace_back` → **one more heap
  allocation per attribute** (`nvtx_span.cpp:47`).

`docs/tracing.md` notes that NVTX ranges are "near-zero-cost no-op stubs" when no
profiler is attached. That is true of the **NVTX API calls**, but the `Span`
layer above them allocates regardless. `NIXL_TRACE_BACKENDS=nvtx` without `nsys`
is *not* free. The genuinely free configuration is `-Dwith_trace=false`, where
the call-site macros compile away.

### The decisive difference is granularity, not per-event cost

- Telemetry: **one event batch per transfer** — O(1).
- Tracing at the resolution this investigation needs: **one span per
  `fi_writemsg`** — O(batch).

At batch 1024 that is ~1024 spans ≈ 2048 heap allocations inside a single
`postXfer`. The overhead scales with precisely the variable under study, so it
does not add a constant — **it manufactures the trend being hunted.**

Telemetry is therefore the right vehicle. NVTX retains one narrow use: ordering
and causality questions at small batch, if the aggregate numbers leave ambiguity.

---

## 3. The per-post time budget

At 40 GB/s line rate with 16 KB messages:

| Convention | Posts/sec | Interval |
|---|---|---|
| 40×10⁹ B/s ÷ 16384 B | 2,441,406 | **409.6 ns** |
| 40×10⁹ B/s ÷ 16000 B | 2,500,000 | **400.0 ns** |
| 40×2³⁰ B/s ÷ 16384 B | 2,621,440 | **381.5 ns** |

**~400 ns per post**, and two properties of that number drive the whole design:

1. **It is per descriptor, not per `postXfer`.** Each 16 KB message is one
   `fi_writemsg`, i.e. one descriptor. A batch-B transfer gets B × 400 ns.
2. **It is not NIXL's to spend.** It must also cover `fi_writemsg` itself (a few
   hundred ns on EFA) plus completion processing. NIXL's own per-descriptor
   overhead budget is realistically **~100–250 ns**.

With PT OFF this all lands on one thread, which round-robins posts across all
rails. Spreading work over rails does not relax the per-post interval for that
thread.

---

## 4. Design: accumulate in the loop, emit once per transfer

Telemetry is a metrics pipeline (counters, gauges, histograms aggregated
off-thread), not a per-call tracer. That suits the question — *where does time
go* and *how does it scale with batch* are distribution questions.

The constraint is that per-descriptor events would mean O(batch) pushes through
the staging mutex. The way around it:

> **Accumulate phase costs into plain locals inside the descriptor loop, then
> emit one batch of events per transfer.**

This preserves telemetry's O(1), zero-allocation export path while still
yielding inner-phase resolution.

### Outer phases — timers outside the descriptor loop

Two timestamp reads each, per `postXfer`, independent of batch size. The
`NIXL_POST_PROFILE` column is the selector name (§6.5).

| Phase | `NIXL_POST_PROFILE` | Location | Expected batch scaling |
|---|---|---|---|
| P1 connection lookup + state check | `conn_lookup` | `libfabric_backend.cpp`, `postXfer` | flat |
| P2 per-post setup: notif fragmentation, xfer id, request-tracking sizing | `notif_prep` | `libfabric_backend.cpp`, `postXfer` | O(n) |
| P3 metadata validation pre-pass | `md_validate` | `libfabric_backend.cpp`, `postXfer` | O(n) |
| P4 FI_MORE last-desc pre-pass | `fi_more_prepass` | `libfabric_backend.cpp`, `postXferDescriptors` | O(n) |
| P5 descriptor submit loop (total) | `submit_loop` | `libfabric_backend.cpp`, `postXferDescriptors` | O(n) |
| P6 `notifSendPriv` | `notif_send` | `libfabric_backend.cpp`, `postXfer` | flat — **control** |
| **P7 `progressActiveRails` tail** | `progress_tail` | `libfabric_backend.cpp`, `postXfer` | O(pending) — **suspect** |

P2 turned out to be O(n) rather than flat as the plan assumed: it also sizes the
request-tracking table, which is `descCount × railCount`. That makes it a
candidate explanation for the trend in its own right.

**P1–P7 do not tile `postXfer` — they are not a partition, and `ΣP` is not the
post.** What sits in the gaps between them, deliberately:

- Argument marshalling, the handle cast and its null check, `descCount()`, and
  two `NIXL_DEBUG` statements between P1 and P2.
- `reserveBaseOffset()` (one atomic RMW) and the `use_post_pool` decision, between
  P3 and the submit call.
- `adjust_total_submitted_requests()` and a `NIXL_DEBUG`, between P5 and P6.
- `is_completed()` after P7.
- The whole thread-pool branch, which is unsupported (§6.5).

Each is O(1) in batch size, so none can produce the trend under investigation,
and timing them would cost more than they take. **The check that this is safe is
the residual:** `AGENT_XFER_POST_TIME − ΣP` should be small and, crucially, *flat
in batch size*. A residual that grows with batch means something O(n) is
happening outside every phase and the map is wrong — chase that before
interpreting any phase.

### Inner accumulators — summed across the loop, emitted once

| Accumulator | `NIXL_POST_PROFILE` | Location |
|---|---|---|
| A1 rail selection (`batchingRail` + `useFiMore`) | `rail_select` | `libfabric_backend.cpp`, `postXferDescriptors` |
| A2 request pool allocation | `req_alloc` | `libfabric_rail_manager.cpp`, `prepareAndSubmitTransfer` (both the round-robin and striping paths) |
| A3 `fi_writemsg`, every attempt, endpoint mutex included | `fi_writemsg` | `libfabric_rail.cpp`, `postWrite` |
| **A4 EAGAIN retry: inline CQ drain** | `eagain_drain` | `libfabric_rail.cpp`, `postWrite` — **suspect** |
| A5 `cudaSetDevice` / `vramApplyCtxEx` | `cuda_ctx` | `libfabric_backend.cpp`, `postXferDescriptors` (once per call plus once per device change) |

A3 deliberately includes acquiring `ep_mutex_`. On the single-thread PT-OFF path
that lock should be uncontended and near-free, so a large A3 points at libfabric,
while an A3 that is large *only* when other threads are posting points at the
lock. Excluding the lock would forfeit both readings.

### Counters (no timestamps, ~1 ns each)

`eagain_attempts`, `eagain_max_attempts`, `submitted_requests`, `rails_touched`,
plus the `calibration` series. These are integer adds rather than timestamps, and
they are **always active whenever any phase is** (§6.5) — a phase number cannot
be read without them. A per-descriptor cost only explains a batch-size trend if
`submitted_requests` actually tracks batch size, and a phase is only above the
noise floor if it is well clear of `calibration`.

---

## 5. Overhead budget

`steady_clock::now()` costs ~20–25 ns because `clock_gettime(CLOCK_MONOTONIC)` is
served from the vDSO — the kernel maps a shared object into the process that
reads the TSC in user mode, with no syscall.

> **Verify the clocksource on every test node.** The vDSO fast path only applies
> when the clocksource is TSC-based. On `acpi_pm` or `xen` the call becomes a real
> syscall at ~500+ ns and every estimate below is wrong by ~20×.
>
> ```bash
> cat /sys/devices/system/clocksource/clocksource0/current_clocksource   # want: tsc
> ```

**Outer phases (P1–P7):** seven non-contiguous phases need up to two boundary
reads each; sharing boundaries where they abut gives ~10 reads per `postXfer`,
so ~250 ns per transfer — amortized over the batch:

| Batch | Cost per descriptor | Share of 400 ns |
|---|---|---|
| 16 | ~16 ns | ~4% |
| 64 | ~4 ns | ~1% |
| 256 | ~1 ns | ~0.2% |
| 1024 | ~0.25 ns | ~0.06% |

**All seven outer phases can therefore share a single run at batch ≥ 64**, and
the amortization improves at exactly the large batch sizes where the problem
lives.

**Inner accumulators (A1–A5):** ten reads per descriptor, no amortization. Use
`__rdtsc()` (~6–8 ns, unserialized) rather than `steady_clock`, giving ~60–80 ns
per descriptor — a large fraction of NIXL's own ~100–250 ns budget. Unaffordable
in aggregate, hence the run protocol below.

**One accumulator per run is the affordable unit:** a single timestamp pair at
~12–16 ns per descriptor on the rdtsc path, which is what `NIXL_POST_PROFILE`
(§6.5) exists to enforce. Selecting `all` costs the full ~60–80 ns and produces
numbers that describe the instrument; it is there for smoke-testing the harness,
not for measurement.

---

## 6. Code changes (as built)

### 6.1 New event types

`src/core/telemetry/telemetry_event.h`. Three constraints:

- `AGENT_TELEMETRY_EVENTS_DROPPED` must remain the last enumerator —
  `nixl_telemetry_event_type_count` derives from it (`:101`) and a `static_assert`
  enforces the invariant (`:154`).
- New types therefore go **before** it, which shifts its numeric value. That is a
  wire-format change: `TELEMETRY_VERSION` (`:32`) must be bumped and
  `examples/python/telemetry_reader.py` updated to match.
- Each new type needs an entry in three places: the enum, the
  `telemetry_metric_event_types` array (`:125`), and `telemetryMetricDescriptor()`
  (`:335`) for its counter/gauge/histogram series names.

**Decision taken:** insert before `DROPPED` and bump the version. 17 event types
were added as 20–36, `AGENT_TELEMETRY_EVENTS_DROPPED` moved to 37, and
`TELEMETRY_VERSION` went 4 → 5. Appending after `DROPPED` would have kept the
wire format but left the enum in an order that no longer means anything, and the
version field exists precisely so that this is a supported change. Everything
that mirrors the enum was updated with it: `telemetry_metric_event_types`,
`telemetryEventTypeStr`, `telemetryErrorStatusLabel`, the descriptor table, the
`prometheus_mp` shared-memory schema (`MP_STORE_SCHEMA_VERSION` 1 → 2, slot count
21 → 38, layout `static_assert` 6800 → 11696 bytes), `test/gtest/telemetry_test.cpp`,
and `examples/python/telemetry_reader.py`.

Two derived constants keep the rest of the harness from hard-coding anything:
`nixl_post_phase_first_event` and `nixl_post_phase_event_count`.

### 6.2 Nanosecond units

`addXferStats()` takes `std::chrono::microseconds` and truncates, which is
useless for sub-microsecond phases. `nixlTelemetry::addPostPhaseStats()` was
added alongside it, taking **nanoseconds**. It takes the raw
`std::array<uint64_t, nixl_post_phase_event_count>` rather than a backend type,
so telemetry keeps no dependency on the backend API, and it publishes the whole
block as one all-or-none batch through `tryPushBatch` — one staging-queue
operation per post regardless of how many phases are active.

### 6.3 Histogram buckets

Defaults start at 10 µs (`src/plugins/telemetry/common/histogram_buckets.h:40`),
far too coarse. Override with `NIXL_TELEMETRY_HISTOGRAM_BUCKETS_US`, which parses
doubles, so sub-microsecond boundaries are expressible:

```bash
export NIXL_TELEMETRY_HISTOGRAM_BUCKETS_US=0.25,0.5,1,2,4,8,16,32,64,128
```

**Unit mismatch, resolved per metric rather than per exporter.** That variable is
microsecond-denominated and its bounds are applied to the observed value
directly, so nanosecond-valued phases would land 1000× off. Neither option the
plan offered was taken. Instead the metric descriptor gained one field:

```cpp
double histogramScaleToUs = 1.0;   // ns-valued events set 0.001
```

The event keeps its native nanosecond value — counters, gauges, and the raw
export stay in ns, which is what a reader wants — and only the bucket lookup is
scaled. Every exporter applies it (`prometheus`, `prometheus_mp`, `doca`), so all
three share one bucket table and one set of bounds. The default initializer means
no pre-existing descriptor row changed.

This is strictly better than either alternative: µs-as-`double` would have thrown
away the exact integer the timer produced, and a second `..._BUCKETS_NS`
variable would have forced whoever reads the output to know which series is
denominated in which unit.

### 6.4 Getting numbers out of the plugin

The telemetry object is agent-owned (`nixlAgentData::telemetry_`) and is **not**
handed to backends. `nixlBackendEngine` does already have a telemetry channel —
`addTelemetryEvent()` / `getTelemetryEvents()` — but it is **the wrong instrument
here**, which is worth stating plainly because reaching for it is the obvious
move: it takes a mutex and appends to a growing vector *per event*, and the
per-descriptor accumulators fire once per descriptor. Its cost would therefore
scale with batch size and manufacture the very trend under investigation, inside
a ~400 ns budget.

What was built instead:

- **`nixlPostPhaseSamples`** (`backend/backend_engine.h`) — a fixed POD,
  `std::array<uint64_t, nixl_post_phase_event_count>` plus `add`, `setMax`,
  `clear`, `empty`. Accumulation is lock-free, allocation-free, and O(1) in
  descriptor count.
- **`virtual bool drainPostPhaseSamples(nixlPostPhaseSamples &) const`** —
  default returns `false`, so every other backend is unaffected. It is `const`
  because `postXfer` is, and because the agent drains on the *same thread* that
  ran `postXfer`, a backend's accumulator can be a plain `thread_local` with no
  synchronization at all.
- **A separate call site in `postXferReq`**, not an addition to
  `updateRequestStats()`. That path deliberately publishes nothing for
  `NIXL_TELEMETRY_POST`, deferring to completion so that one transfer yields one
  set of events — whereas phase samples describe the post itself and have no
  completion to wait for. Draining at the new site also covers the error path: a
  post that failed partway still spent time doing so, and dropping it would bias
  the histograms toward successful posts.

The backend stays telemetry-agnostic and the agent owns all publishing. This is
cheaper and less invasive than the tracer plumbing that the NVTX work needed
(which had to thread a tracer through `backend_aux.h` into the rails).

### 6.5 The `NIXL_POST_PROFILE` selector

`src/utils/libfabric/libfabric_post_profile.{h,cpp}`. One environment variable
decides which phases are live for a run:

```bash
export NIXL_POST_PROFILE=submit_loop                  # short name
export NIXL_POST_PROFILE=agent_post_phase_submit_loop # full event name
export NIXL_POST_PROFILE='*submit*'                   # fnmatch glob
export NIXL_POST_PROFILE=conn_lookup,progress_tail    # comma-separated list
export NIXL_POST_PROFILE=all                          # everything (§5: too expensive to trust)
```

Behaviour worth knowing before you rely on it:

- **Unset means genuinely inert.** No timestamps, no accumulation, and
  `drainPostPhaseSamples()` returns `false` rather than publishing a block of
  zeros — so an unprofiled run is not silently recorded as "all phases took 0 ns".
- **Whitespace is trimmed and duplicates collapse.** A token that matches nothing
  is reported with a warning and ignored; if *nothing* matches, profiling stays
  off with a second warning rather than falling back to everything.
- **The five counter series are added to any non-empty selection** (§4), so a
  phase number always arrives with its denominator and its noise floor.
- **The clock is chosen once, at engine construction.** `__rdtsc()` (~6–8 ns)
  where the architecture has it *and* `/sys/.../current_clocksource` reads `tsc`;
  otherwise `steady_clock` (~20–25 ns), with a warning saying which you got. The
  clocksource check is not decoration: under `hpet` or `acpi_pm` the TSC is not
  guaranteed constant or synchronized across cores, and a phase timed across a
  core migration would be garbage rather than merely noisy.
- **Selector parsing and TSC calibration happen once**, in the engine
  constructor. The post path only ever reads an already-resolved immutable mask.
- **The post thread pool is not supported.** Samples taken on pool threads are
  never drained, because the drain happens on the thread the agent called. If the
  pool is active and profiling is on, you get a one-time warning saying the
  numbers cover only the calling thread's share. Set the post thread count to 0
  for a measurement run — the single-thread path is the case under study anyway.

Reading `NIXL_POST_PROFILE` uses plain `getenv` rather than `nixl::config`,
matching the rest of `src/utils/libfabric` (see `getCustomStringParam` in
`libfabric_common.cpp`) and keeping that layer's dependencies unchanged;
`nixl::config` would pull `toml++` into `libfabric_utils_deps` for a debug knob.

---

## 7. Run protocol

Because inner phases cannot share a run, phases come from *different* runs. Two
additions make that rigorous.

### 7.1 Calibration (automatic, every run)

The plan called for a dedicated calibration run timing a zero-length region. That
became unnecessary: `agent_post_phase_calibration` measures exactly that — one
timestamp pair with nothing between them — and it is recorded on **every**
profiled post, because it is one of the always-on series (§4). So the harness
cost is measured under the same conditions as the phase it is being subtracted
from, in the same run, rather than inferred from an earlier one.

Use it two ways:

- **Subtract it.** A phase's real cost is its reported value minus one calibration
  sample per timestamp pair the phase contains (one for an outer phase; one per
  descriptor for an accumulator, so multiply by `submitted_requests`).
- **As a floor.** Any phase within ~2× of the calibration figure is not
  measurable with this instrument. Do not report it as small; report it as below
  the noise floor.

Calibration also cross-checks the clock: it should land near 6–8 ns on the rdtsc
path and 20–25 ns on `steady_clock`. A wildly different number means the clock is
not what the startup log claimed and nothing else from that run should be
trusted.

### 7.2 Cross-run comparability

The `ΣP ≈ postDuration` self-consistency check becomes a *cross-run* check, and
it is what validates the exercise:

- **Anchor every run on total post latency.** `AGENT_XFER_POST_TIME` is already
  recorded and costs nothing extra. If total post latency varies across runs by
  more than a few percent, the runs are not comparable and phases must not be
  summed.
- **Report each phase as a fraction of its own run's total**, not as absolute
  nanoseconds. Fractions survive run-to-run drift; absolute values do not.
- **Interleave, do not block.** Run A,B,C,A,B,C rather than AAA,BBB,CCC so
  thermal or allocation drift is not misattributed to a phase.
- Hold everything else fixed: same node pair, pinned cores, locked GPU clocks,
  identical iteration counts, `tsc` clocksource confirmed on both nodes.

### 7.3 Schedule

| Run | `NIXL_POST_PROFILE` | Cost | Purpose |
|---|---|---|---|
| 1 | `agent_post_phase_*` (P1–P7) | ~1% at B ≥ 64 | Locate the dominant region |
| 2 | `eagain_attempts` | ~1 ns/desc | EAGAIN incidence vs. batch size |
| 3+ | one of `rail_select` / `req_alloc` / `fi_writemsg` / `eagain_drain` / `cuda_ctx` | ~15 ns/desc | Only inside the region run 1 implicates |

Runs 1 and 2 are cheap and may settle the question on their own: if run 1 shows
the `progressActiveRails` tail (P7) and the submit loop (P5) growing
superlinearly, and run 2 shows EAGAIN attempts climbing with batch, the
backpressure hypothesis is confirmed without a single per-descriptor timer.

**Run 2 needs no timestamps at all.** Note that `eagain_attempts` and
`eagain_max_attempts` are always-on, so *any* selection reports them — run 2 is
just the cheapest way to get them, by naming a counter and nothing else. Keep
them **unsampled**: EAGAIN incidence is position-dependent within a batch (late
descriptors meet a full send queue), so sampling would suppress the very signal
being sought. The harness does not sample and has no sampling knob, deliberately.

For runs 3+, note that `submit_loop` (P5) is the aggregate the accumulators
decompose. Selecting `submit_loop` alongside one accumulator in the same run
costs one extra timestamp pair per post and gives you the total to check the
accumulator against; that is usually worth it.

### 7.4 Sweep and configuration

```bash
export NIXL_TELEMETRY_ENABLE=1
export NIXL_TELEMETRY_DIR=/dev/shm/nixltel          # node-local, never NFS
export NIXL_TELEMETRY_BUFFER_SIZE=1048576           # sizes the staging queue AND the ring file
export NIXL_TELEMETRY_ENABLED_METRICS="agent_xfer_post_time,agent_post_*,agent_telemetry_events_dropped"

# Which phases are timed. One phase (or one small group) per run -- see 7.3.
export NIXL_POST_PROFILE="agent_post_phase_*"

# PT OFF is the default: --enable-pt is a bare boolean flag, so simply omit it.
./nixlbench --backend=LIBFABRIC --op-type=WRITE \
  --start-block-size=16384 --max-block-size=16384 \
  --start-batch-size=64 --max-batch-size=64 \
  --num-threads=1 --progress-threads=0 --warmup-iter=100 --num-iter=1000 \
  --check-consistency=1
```

Both iteration counts are rounded **up** to a multiple of
`num_threads * large_blk_iter_ftr` (`utils.cpp:747`), i.e. 16 here, so the run
reports `num_iter=1008, warmup_iter=112` — the numbers to expect in the printed
configuration and in the sample counts. The large-block divisor
(`utils.cpp:1347`) applies above 1 MiB only, so a 16 KiB block runs the full
count.

Setting `NIXL_TELEMETRY_DIR` selects the built-in buffer exporter, which writes
**raw per-event samples** to `$NIXL_TELEMETRY_DIR/<agent_name>` — full
distributions offline, no Prometheus to stand up.
`NIXL_TELEMETRY_HISTOGRAM_BUCKETS_US` is a Prometheus-exporter setting and does
nothing on this route. Default `NIXL_TELEMETRY_BUFFER_SIZE` is 4096 events,
which both drops at the staging queue and wraps the ring; oversize it and check
the drop gate below. If the library is installed somewhere other than its build
prefix, set `NIXL_PLUGIN_DIR=<prefix>/lib/<triplet>/plugins` too, or the backend
plugin is not found.

**Two separate filters, and both must let a series through.**
`NIXL_POST_PROFILE` decides what is *measured* (it is what costs runtime);
`NIXL_TELEMETRY_ENABLED_METRICS` decides what is *exported*. Use
`agent_post_*` — not `agent_post_phase_*` — for the allowlist, or the
accumulators (`agent_post_accum_*`) and counters (`agent_post_eagain_*`,
`agent_post_submitted_requests`, `agent_post_rails_touched`) are measured and then
silently discarded. Both variables use `fnmatch` against base event names, and in
both a token matching nothing is ignored with a warning
(`docs/telemetry.md:155`), so check the startup log rather than assuming a glob
landed.

One invocation sweeps the batch range (`main.cpp:115`), but **a swept run is not
analysable on the buffer route**: `nixlTelemetryEvent` is `{eventType_, value_}`
with no timestamp, so samples from different batch sizes cannot be told apart
afterwards. Pin one batch size per invocation
(`--start-batch-size == --max-batch-size`) with a fresh `NIXL_TELEMETRY_DIR` per
point. Keep `--warmup-iter` identical across runs — it is part of the
comparability protocol in §7.2.

### 7.5 Reading the samples

`examples/python/postxfer_profile_report.py <buffer file>` aggregates the ring
into count/mean/p50/p99/max per event and prints the validity gates (staging
drops, phase coverage against `agent_xfer_post_time`, per-descriptor post against
the 400 ns budget, and the calibration noise floor). Unlike
`examples/python/telemetry_reader.py` it does not consume the buffer, so it can
run on a file left behind by an exited process.

Expect exactly `warmup_iter + num_iter` samples per selected series (1120 for the
configuration above). A shortfall with the drop gate at zero means samples were
staged but never exported — the teardown flush in `nixlTelemetry::~nixlTelemetry`
exists for that reason; before it, a run that ended between two 100 ms flushes
lost its tail silently (701 of 1120 observed).

Pass `--skip <warmup_iter + a few>`, using the *rounded* warmup count (112, not
100), or warmup samples leak in. Warmup posts are in the buffer too, and the
**first post pays connection establishment** — measured at 2.04 s inside
`agent_post_phase_conn_lookup`, against a 60 ns median for that same phase. One
such sample decides any mean or sum it lands in, which is why the coverage gate
is computed on medians.

Overhead isolation, per `docs/telemetry.md:159` — run each configuration four
ways and publish the deltas alongside results:

1. telemetry off,
2. telemetry on, `NIXL_POST_PROFILE` unset (the harness inert — this pair
   isolates telemetry's own cost from the profiling harness's),
3. telemetry on with `NIXL_TELEMETRY_EXPORTER=NOP` (collects, discards output),
4. the phase selection under test.

The difference in reported post latency **is** the observer effect. Step 2 is the
one to watch: if 1 → 2 already moves post latency, the baseline is not a baseline
and no phase attribution from step 4 is safe.

---

## 8. Interpretation

| Observation | Conclusion |
|---|---|
| P7 + A4 dominate and grow superlinearly | Backpressure inversion: with PT OFF nothing drains the CQ until the end of `postXfer`, so once in-flight posts exceed TX queue depth every further `fi_writemsg` returns `-FI_EAGAIN` and drains inline. Post stops being "submit" and absorbs wire time. Fix is decoupling, not micro-optimization. |
| P3 + P4 + A1 dominate | CPU-side per-descriptor overhead. Two redundant O(n) pre-passes, and rail selection computed twice per descriptor (`batchingRail` in the FI_MORE pre-pass at `:1318` and again in the main loop at `:1326`). Directly fixable. |
| A3 dominates and is flat per descriptor | At the provider's floor. Levers are FI_MORE batch size (hardcoded 16, `libfabric_common.h:47`) and queue depth, not NIXL code. |
| A5 non-trivial | CUDA context/device handling on the post path; candidate for hoisting out of the loop. |

**Leading hypothesis** is the first row: the EAGAIN retry loop in
`nixlLibfabricRail::postWrite` plus the `progressActiveRails` tail at the end of
`postXfer`. Both are inline CQ progress on the posting thread under PT OFF, and
both scale with the number of outstanding completions, which scales with batch.
The predicted signature is post latency roughly flat until batch ≈ TX queue
depth, then a sharp knee, with `eagain_drain` and `eagain_attempts` both picking
up exactly at the knee.

**The hypothesis is falsifiable, and that is the point of run 2.** If
`eagain_attempts` stays at zero across the knee, backpressure inversion is wrong:
the post never waited on a full queue, so the cost is CPU-side and lives in one of
P2/P3/P4/A1. Record that outcome as a result rather than looking for a way to
keep the hypothesis.

---

## 9. Limitations

- **Aggregated distributions, not per-call timelines.** This shows that a phase
  grew, not *which* descriptor in a batch stalled. If the answer turns out to
  depend on ordering within a single post, that is when NVTX at small batch earns
  its place.
- **Phases from different runs.** Mitigated by §7.1 and §7.2, not eliminated.
- **`__rdtsc()` is x86-only** and assumes an invariant TSC. Guarded, with a
  `steady_clock` fallback elsewhere and on non-`tsc` clocksources — but on
  aarch64 every phase costs ~3× more to measure, so the affordability arithmetic
  in §5 is x86 arithmetic.
- **Write path only for A3/A4.** The read path posts through `fi_read`, which
  would need its own series to be reported honestly rather than folded into a
  `fi_writemsg` number. Read posts report A2 but not A3/A4. `--op_type=WRITE` is
  the case under investigation; a read investigation needs new events first.
- **Post thread pool unsupported.** Samples taken on pool threads are never
  drained (§6.5). Profiling a pooled run silently under-reports; the harness warns
  once, but the only correct answer is to set the post thread count to 0.
- **Instrumentation is not free**, only affordable. Always publish the §7.4
  overhead deltas next to any result.

---

## 10. Decisions

**Settled during implementation:**

1. **Wire format** — inserted the new event types before
   `AGENT_TELEMETRY_EVENTS_DROPPED` and bumped `TELEMETRY_VERSION` 4 → 5. See
   §6.1 for the full list of things that had to move with it.
2. **Histogram units** — neither option as posed. Events keep native nanoseconds
   and the metric descriptor carries a `histogramScaleToUs` factor applied at
   bucket lookup only, so all three exporters share one bucket table. See §6.3.

**Still open:**

3. **Branch base:** `origin/main`, or the local libfabric development branch that
   the eventual fix will land on?
4. **Upstreamability.** The event-type additions are generic (any backend can
   implement `drainPostPhaseSamples`), but the phase *names* describe the
   libfabric post path specifically. If this goes upstream, that naming is the
   thing to argue about first.
