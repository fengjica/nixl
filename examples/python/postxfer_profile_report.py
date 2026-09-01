#!/usr/bin/env python3
"""Aggregate a NIXL telemetry ring buffer into per-event statistics.

Reads the buffer written by the default (buffer) telemetry exporter, i.e.
$NIXL_TELEMETRY_DIR/<agent_name>, and prints count/mean/p50/p99/max per event
type. Unlike examples/python/telemetry_reader.py this does not consume the
buffer: it reads the header, walks read_pos..write_pos, and leaves the buffer
untouched, so it can run against a file left behind by an exited process.
"""
import argparse
import ctypes
import mmap
import os
import statistics
import sys

TELEMETRY_VERSION = 5

EVENT_NAMES = [
    "agent_tx_bytes",
    "agent_rx_bytes",
    "agent_tx_requests_num",
    "agent_rx_requests_num",
    "agent_memory_registered",
    "agent_memory_deregistered",
    "agent_xfer_time",
    "agent_xfer_post_time",
    "agent_err_not_posted",
    "agent_err_invalid_param",
    "agent_err_backend",
    "agent_err_not_found",
    "agent_err_mismatch",
    "agent_err_not_allowed",
    "agent_err_repost_active",
    "agent_err_unknown",
    "agent_err_not_supported",
    "agent_err_remote_disconnect",
    "agent_err_canceled",
    "agent_err_no_telemetry",
    "agent_post_phase_conn_lookup",
    "agent_post_phase_notif_prep",
    "agent_post_phase_md_validate",
    "agent_post_phase_fi_more_prepass",
    "agent_post_phase_submit_loop",
    "agent_post_phase_notif_send",
    "agent_post_phase_progress_tail",
    "agent_post_accum_rail_select",
    "agent_post_accum_req_alloc",
    "agent_post_accum_fi_writemsg",
    "agent_post_accum_eagain_drain",
    "agent_post_accum_cuda_ctx",
    "agent_post_phase_calibration",
    "agent_post_eagain_attempts",
    "agent_post_eagain_max_attempts",
    "agent_post_submitted_requests",
    "agent_post_rails_touched",
    "agent_telemetry_events_dropped",
]

# Values in nanoseconds (everything the postXfer harness times); the two
# built-in xfer series are microseconds, the counters are unitless.
NS_EVENTS = {
    n
    for n in EVENT_NAMES
    if n.startswith("agent_post_phase_") or n.startswith("agent_post_accum_")
}
US_EVENTS = {"agent_xfer_time", "agent_xfer_post_time"}


class Header(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("write_pos", ctypes.c_size_t),
        ("read_pos", ctypes.c_size_t),
        ("version", ctypes.c_uint32),
        ("expected_version", ctypes.c_uint32),
        ("capacity", ctypes.c_size_t),
        ("mask", ctypes.c_size_t),
    ]


class Event(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("event_type", ctypes.c_uint32),
        ("_padding", ctypes.c_char * 4),
        ("value", ctypes.c_uint64),
    ]


def name_of(event_type):
    if 0 <= event_type < len(EVENT_NAMES):
        return EVENT_NAMES[event_type]
    return f"unknown({event_type})"


def read_events(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), size, access=mmap.ACCESS_READ)
    hdr = Header.from_buffer_copy(mm[: ctypes.sizeof(Header)])
    if hdr.version != TELEMETRY_VERSION:
        print(
            f"warning: buffer version {hdr.version} != expected {TELEMETRY_VERSION}",
            file=sys.stderr,
        )
    base = ctypes.sizeof(Header)
    esize = ctypes.sizeof(Event)
    available = hdr.write_pos - hdr.read_pos
    wrapped = available > hdr.capacity
    if wrapped:
        start = hdr.write_pos - hdr.capacity
        available = hdr.capacity
    else:
        start = hdr.read_pos
    events = []
    for i in range(available):
        slot = (start + i) & hdr.mask
        off = base + slot * esize
        ev = Event.from_buffer_copy(mm[off : off + esize])
        events.append((ev.event_type, ev.value))
    mm.close()
    return hdr, events, wrapped


def pct(sorted_vals, q):
    if not sorted_vals:
        return 0
    k = min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1))))
    return sorted_vals[k]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", help="telemetry buffer file ($NIXL_TELEMETRY_DIR/<agent>)")
    ap.add_argument("--csv", action="store_true", help="emit CSV instead of a table")
    ap.add_argument(
        "--batch", default="", help="batch size label to carry into the CSV output"
    )
    ap.add_argument(
        "--skip",
        type=int,
        default=0,
        help="drop the first N samples of every event type. The first post pays "
        "connection establishment (seconds, in conn_lookup), so warmup samples "
        "have to go before any mean is meaningful.",
    )
    args = ap.parse_args()

    hdr, events, wrapped = read_events(args.path)
    by_type = {}
    for et, val in events:
        by_type.setdefault(et, []).append(val)
    if args.skip:
        by_type = {et: v[args.skip :] for et, v in by_type.items() if len(v) > args.skip}

    if not args.csv:
        print(f"file:     {args.path}")
        print(f"version:  {hdr.version}  capacity: {hdr.capacity}")
        print(f"events:   {len(events)}  wrapped: {wrapped}")
        if wrapped:
            print("WARNING: buffer wrapped - oldest samples were overwritten")
        print()
        print(
            f"{'event':34s} {'unit':4s} {'count':>8s} {'mean':>12s} "
            f"{'p50':>12s} {'p99':>12s} {'max':>12s} {'sum':>14s}"
        )

    rows = []
    for et in sorted(by_type):
        name = name_of(et)
        vals = sorted(by_type[et])
        unit = "ns" if name in NS_EVENTS else ("us" if name in US_EVENTS else "-")
        row = (
            name,
            unit,
            len(vals),
            statistics.fmean(vals),
            pct(vals, 0.50),
            pct(vals, 0.99),
            vals[-1],
            sum(vals),
        )
        rows.append(row)
        if not args.csv:
            print(
                f"{row[0]:34s} {row[1]:4s} {row[2]:8d} {row[3]:12.1f} "
                f"{row[4]:12d} {row[5]:12d} {row[6]:12d} {row[7]:14d}"
            )

    if args.csv:
        print("batch,event,unit,count,mean,p50,p99,max,sum")
        for r in rows:
            print(
                f"{args.batch},{r[0]},{r[1]},{r[2]},{r[3]:.1f},{r[4]},{r[5]},{r[6]},{r[7]}"
            )

    # Validity gates that are cheap to check here.
    if not args.csv:
        print()
        dropped = by_type.get(EVENT_NAMES.index("agent_telemetry_events_dropped"), [])
        print(f"gate: staging drops = {sum(dropped)} (must be 0)")
        post = by_type.get(EVENT_NAMES.index("agent_xfer_post_time"), [])
        # A counters-only run (NIXL_POST_PROFILE=calibration) has no phase series,
        # and a coverage number computed over none of them would read as 0% covered
        # rather than "not measured".
        have_phases = any(
            by_type.get(EVENT_NAMES.index(n))
            for n in NS_EVENTS
            if n != "agent_post_phase_calibration"
        )
        if post:
            # Medians, not sums: one connection-establishing post costs seconds and
            # would otherwise decide the coverage number by itself.
            post_p50_ns = pct(sorted(post), 0.50) * 1000.0
            if have_phases:
                phase_p50_ns = sum(
                    pct(sorted(by_type.get(EVENT_NAMES.index(n), [])), 0.50)
                    for n in EVENT_NAMES
                    if n in NS_EVENTS and n != "agent_post_phase_calibration"
                )
                print(
                    f"gate: p50 sum(phases) = {phase_p50_ns / 1000.0:.2f} us vs "
                    f"p50 agent_xfer_post_time = {post_p50_ns / 1000.0:.2f} us "
                    f"({100.0 * phase_p50_ns / post_p50_ns:.1f}% covered, "
                    f"residual {100.0 * (post_p50_ns - phase_p50_ns) / post_p50_ns:.1f}%)"
                )
            submitted = pct(
                sorted(
                    by_type.get(EVENT_NAMES.index("agent_post_submitted_requests"), [1])
                ),
                0.50,
            )
            print(
                f"gate: per-descriptor p50 post = "
                f"{post_p50_ns / max(1, submitted):.0f} ns "
                f"(budget 400 ns at 40 GB/s x 16 KiB)"
            )
        cal = by_type.get(EVENT_NAMES.index("agent_post_phase_calibration"), [])
        if cal:
            print(
                f"gate: calibration (one timestamp pair) mean = "
                f"{statistics.fmean(cal):.1f} ns -> noise floor ~"
                f"{2 * statistics.fmean(cal):.0f} ns"
            )


if __name__ == "__main__":
    sys.exit(main())
