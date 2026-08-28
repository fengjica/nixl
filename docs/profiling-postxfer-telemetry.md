# Profiling the `postXfer` Path with Telemetry

Plan for locating where time is spent between `nixlAgent::postXferReq()` and
`fi_writemsg()` in the libfabric backend, using the telemetry subsystem as the
measurement vehicle.

**Context:** NCCLOFI-1865 (gpt-oss perf headroom analysis). `nixlbench` reports
post latency growing with batch size on the **progress-thread-off (PT OFF)**
path, so the whole path of interest runs on a single thread.

**Scope:** PT OFF, `NIXL_WRITE`, single posting thread. The PT ON path defers
posts through the ring buffer and is a different problem.

All file/line references were verified against `origin/main` at `fa01dd3a`.
Function names are the durable anchors; line numbers will drift.

---

## 1. The path under measurement

```
nixlAgent::postXferReq()                    src/core/nixl_agent.cpp:1084
  └─ nixlLibfabricEngine::postXfer()        src/plugins/libfabric/libfabric_backend.cpp:1396
       └─ postXferDescriptors()             src/plugins/libfabric/libfabric_backend.cpp:1278
            └─ railManager::prepareAndSubmitTransfer()
                                            src/utils/libfabric/libfabric_rail_manager.cpp:376
                 └─ nixlLibfabricRail::postWrite()
                                            src/utils/libfabric/libfabric_rail.cpp:1345
                      └─ fi_writemsg()      src/utils/libfabric/libfabric_rail.cpp:1393
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

Two timestamp reads each, per `postXfer`, independent of batch size.

| Phase | Location | Expected batch scaling |
|---|---|---|
| P1 connection lookup + state check | `libfabric_backend.cpp:1406–1424` | flat |
| P2 notification fragmentation | `libfabric_backend.cpp:1436–1447` | flat |
| P3 metadata validation pre-pass | `libfabric_backend.cpp:1468–1481` | O(n) |
| P4 FI_MORE last-desc pre-pass | `libfabric_backend.cpp:1316–1323` | O(n) |
| P5 descriptor submit loop (total) | `libfabric_backend.cpp:1325–1390` | O(n) |
| P6 `notifSendPriv` | `libfabric_backend.cpp:1597` | flat |
| **P7 `progressActiveRails` tail** | `libfabric_backend.cpp:1612` | O(pending) — **suspect** |

### Inner accumulators — summed across the loop, emitted once

| Accumulator | Location |
|---|---|
| A1 rail selection (`batchingRail` + `useFiMore`) | `libfabric_backend.cpp:1326–1328` |
| A2 request pool allocation | `libfabric_rail_manager.cpp:423` |
| A3 `fi_writemsg` first attempt | `libfabric_rail.cpp:1393` |
| **A4 EAGAIN retry: attempt count + drain time** | `libfabric_rail.cpp:1404–1431` — **suspect** |
| A5 `cudaSetDevice` / `vramApplyCtxEx` | `libfabric_backend.cpp:1297`, `1340–1350` |

### Counters (no timestamps, ~1 ns each)

`eagain_attempts_total`, `eagain_max_attempts_in_one_post`,
`submitted_requests`, `rails_touched`.

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

**Inner accumulators (A1–A5):** six reads per descriptor, no amortization. Use
`__rdtsc()` (~6–8 ns, unserialized) rather than `steady_clock`, giving ~42 ns per
descriptor — **17–40% of NIXL's own ~100–250 ns budget.** Unaffordable in
aggregate, hence the run protocol below.

---

## 6. Code changes required

### 6.1 New event types

`src/core/telemetry/telemetry_event.h`. Three constraints:

- `AGENT_TELEMETRY_EVENTS_DROPPED` must remain the last enumerator —
  `nixl_telemetry_event_type_count` derives from it (`:61`) and a `static_assert`
  enforces the invariant (`:96`).
- New types therefore go **before** it, which shifts its numeric value. That is a
  wire-format change: **bump `TELEMETRY_VERSION` from 4 to 5** (`:30`) and update
  `examples/python/telemetry_reader.py`.
- Each new type needs an entry in three places: the enum, the
  `telemetry_metric_event_types` array (`:84`), and `telemetryMetricDescriptor()`
  (`:218`) for its counter/gauge/histogram series names.

*Alternative, if breaking the wire format is unacceptable:* append after
`DROPPED` and relax the "must remain last" invariant plus its `static_assert`.
Non-breaking but uglier. **Open decision — see §10.**

### 6.2 Nanosecond units

`addXferStats()` takes `std::chrono::microseconds` and truncates, which is
useless for sub-microsecond phases. The event value is already `uint64_t`, so add
a separate `addPostPhaseStats()` taking nanoseconds, with `_ns`-suffixed series
names. **Do not reuse the `_us` plumbing.**

### 6.3 Histogram buckets

Defaults start at 10 µs (`src/plugins/telemetry/common/histogram_buckets.h:40`),
far too coarse. Override with `NIXL_TELEMETRY_HISTOGRAM_BUCKETS_US`, which parses
doubles, so sub-microsecond boundaries are expressible:

```bash
export NIXL_TELEMETRY_HISTOGRAM_BUCKETS_US=0.25,0.5,1,2,4,8,16,32,64,128
```

**Unit mismatch to resolve.** That variable is microsecond-denominated, and its
bounds are applied to the observed value directly. If §6.2 introduces
nanosecond-valued phase histograms, µs bounds land 1000× off. Pick one:

- keep phase histograms in µs as `double` (buckets work unchanged, sub-µs
  resolution preserved because the value is no longer an integer), or
- add a separate `NIXL_TELEMETRY_HISTOGRAM_BUCKETS_NS` for the ns series.

The first is less code. Either way, **do not** let ns values reach µs buckets.

### 6.4 Getting numbers out of the plugin

The telemetry object is agent-owned (`nixlAgentData::telemetry_`) and is **not**
handed to backends. Rather than plumb it into the plugin:

- Accumulate phase sums into `nixlLibfabricBackendH`, which `postXfer` already
  holds.
- Add one virtual getter on `nixlBackendEngine` that the agent calls immediately
  after `postXfer` returns, feeding `nixlXferReqH::updateRequestStats()`
  (`nixl_agent.cpp:78`).

The backend stays telemetry-agnostic and the agent owns all publishing. This is
cheaper and less invasive than the tracer plumbing that the NVTX work needed
(which had to thread a tracer through `backend_aux.h` into the rails).

---

## 7. Run protocol

Because inner phases cannot share a run, phases come from *different* runs. Two
additions make that rigorous.

### 7.1 Calibration run (mandatory, run first)

Instrumentation compiled in and active, but timing a **zero-length region** (two
adjacent `__rdtsc()` reads). This measures the harness cost itself, to be
subtracted from every other run. Without it, each run carries an unknown and
differing overhead and the results are not comparable.

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

| Run | Active instrumentation | Cost | Purpose |
|---|---|---|---|
| 0 | Calibration (null region) | — | Harness baseline to subtract |
| 1 | All of P1–P7 | ~1% at B ≥ 64 | Locate the dominant region |
| 2 | A4 counters only, no timing | ~1 ns/desc | EAGAIN attempts + max |
| 3+ | One of A1/A2/A3/A5 per run | ~15 ns/desc | Only inside the region run 1 implicates |

Runs 0–2 are cheap and may settle the question on their own: if run 1 shows the
`progressActiveRails` tail (P7) and the submit loop (P5) growing superlinearly,
and run 2 shows EAGAIN attempts climbing with batch, the backpressure hypothesis
is confirmed without a single per-descriptor timer.

**Run 2 needs no timestamps at all** — just a counter increment in the EAGAIN
branch at `libfabric_rail.cpp:1404`. Keep it **unsampled**: EAGAIN incidence is
position-dependent within a batch (late descriptors meet a full send queue), so
sampling would suppress the very signal being sought.

For runs 3+, sampling every 16th descriptor and scaling is acceptable for
*timing* — descriptors are homogeneous — but never for A4's counter.

### 7.4 Sweep and configuration

```bash
export NIXL_TELEMETRY_ENABLE="y"
export NIXL_TELEMETRY_ENABLED_METRICS="agent_xfer_post_time,agent_post_phase_*"
export NIXL_TELEMETRY_HISTOGRAM_BUCKETS_US=0.25,0.5,1,2,4,8,16,32,64,128

# PT OFF is the default: --enable_pt is a bare boolean flag, so simply omit it.
./nixlbench --backend=LIBFABRIC --op_type=WRITE \
  --start_block_size=16384 --max_block_size=16384 \
  --start_batch_size=1 --max_batch_size=1024 \
  --num_threads=1 --warmup_iter=100 --num_iter=1000
```

`NIXL_TELEMETRY_ENABLED_METRICS` globs against **base event names**, and a token
matching nothing is ignored with a warning (`docs/telemetry.md:155`). So
`agent_post_phase_*` only takes effect once §6.1 adds events under that prefix —
choose the new names to share it, or the allowlist silently selects nothing.

One invocation sweeps the batch range (`main.cpp:115`), so a single run per
instrumentation configuration covers the whole curve. Keep `--warmup_iter`
identical across runs — it is part of the comparability protocol in §7.2.

Overhead isolation, per `docs/telemetry.md:159` — run each configuration three
ways and publish the deltas alongside results:

1. telemetry off,
2. telemetry on with `NIXL_TELEMETRY_EXPORTER=NOP` (collects, discards output),
3. full phase detail on.

The difference in reported post latency **is** the observer effect.

---

## 8. Interpretation

| Observation | Conclusion |
|---|---|
| P7 + A4 dominate and grow superlinearly | Backpressure inversion: with PT OFF nothing drains the CQ until the end of `postXfer`, so once in-flight posts exceed TX queue depth every further `fi_writemsg` returns `-FI_EAGAIN` and drains inline. Post stops being "submit" and absorbs wire time. Fix is decoupling, not micro-optimization. |
| P3 + P4 + A1 dominate | CPU-side per-descriptor overhead. Two redundant O(n) pre-passes, and rail selection computed twice per descriptor (`batchingRail` in the FI_MORE pre-pass at `:1318` and again in the main loop at `:1326`). Directly fixable. |
| A3 dominates and is flat per descriptor | At the provider's floor. Levers are FI_MORE batch size (hardcoded 16, `libfabric_common.h:47`) and queue depth, not NIXL code. |
| A5 non-trivial | CUDA context/device handling on the post path; candidate for hoisting out of the loop. |

**Leading hypothesis** is the first row: the EAGAIN retry loop at
`libfabric_rail.cpp:1404–1431` plus the `progressActiveRails` tail at
`libfabric_backend.cpp:1612`. Both are inline CQ progress on the posting thread
under PT OFF, and both scale with the number of outstanding completions, which
scales with batch. The predicted signature is post latency roughly flat until
batch ≈ TX queue depth, then a sharp knee.

---

## 9. Limitations

- **Aggregated distributions, not per-call timelines.** This shows that a phase
  grew, not *which* descriptor in a batch stalled. If the answer turns out to
  depend on ordering within a single post, that is when NVTX at small batch earns
  its place.
- **Phases from different runs.** Mitigated by §7.1 and §7.2, not eliminated.
- **`__rdtsc()` is x86-only** and assumes an invariant TSC. Guard it; fall back
  to `steady_clock` elsewhere.
- **Instrumentation is not free**, only affordable. Always publish the §7.4
  overhead deltas next to any result.

---

## 10. Open decisions

1. **Wire format:** bump `TELEMETRY_VERSION` to 5 and insert new event types
   before `AGENT_TELEMETRY_EVENTS_DROPPED` (clean, breaking), or append after it
   and relax the `static_assert` (non-breaking, uglier)?
2. **Histogram units:** µs-as-`double` phase histograms reusing
   `NIXL_TELEMETRY_HISTOGRAM_BUCKETS_US`, or ns series with a new
   `..._BUCKETS_NS`? See §6.3.
3. **Branch base:** `origin/main`, or the local libfabric development branch that
   the eventual fix will land on?
