#pragma once
// Hot-path counters.
//
// Each thread increments its own plain uint64 -- no atomic RMW, no contention,
// one store per event. Aggregation happens only when a report is produced, and
// reads use std::atomic_ref so the cross-thread read is well-defined rather
// than a data race we agreed not to talk about. The cost of this design is that
// a snapshot can miss an in-flight increment; at the rates these count, that is
// a rounding error, and it is the right trade for keeping the measured path
// identical to the unmeasured one.
#include <atomic>
#include <cstdint>

#include "ge/core/platform.hpp"

namespace ge::metrics {

// std::atomic_ref requires a non-const referent; the const_cast is safe because
// we only ever load through it. This buys a well-defined cross-thread read of a
// counter the owning thread updates with plain stores.
GE_ALWAYS_INLINE std::uint64_t load_relaxed(const std::uint64_t& v) noexcept {
    return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(v))
        .load(std::memory_order_relaxed);
}

// Per-I/O-thread counters. Syscall counts are tracked separately from message
// counts because messages-per-syscall is the batching efficiency number, and
// it is the first thing to check when throughput stops scaling.
struct GE_CACHE_ALIGNED IoCounters {
    std::uint64_t frames_in       = 0;
    std::uint64_t frames_out      = 0;
    std::uint64_t bytes_in        = 0;
    std::uint64_t bytes_out       = 0;
    std::uint64_t read_calls      = 0;
    std::uint64_t write_calls     = 0;
    std::uint64_t writev_calls    = 0;
    std::uint64_t poll_calls      = 0;
    std::uint64_t poll_empty      = 0;   // wakeups that found no work
    std::uint64_t accepts         = 0;
    std::uint64_t closes          = 0;
    std::uint64_t parse_errors    = 0;
    std::uint64_t short_writes    = 0;   // kernel buffer full: backpressure begins here
    std::uint64_t evicted_slow    = 0;   // clients dropped for not draining
    std::uint64_t cmd_ring_full   = 0;   // I/O -> shard backpressure
    std::uint64_t busy_ns         = 0;   // CPU actually spent on work
    std::uint64_t poll_wait_ns    = 0;   // blocked inside poll() with nothing to do
    std::uint64_t spin_polls      = 0;   // zero-timeout polls issued while spinning

    void add(const IoCounters& o) noexcept {
        frames_in += o.frames_in;   frames_out += o.frames_out;
        bytes_in  += o.bytes_in;    bytes_out  += o.bytes_out;
        read_calls += o.read_calls; write_calls += o.write_calls;
        writev_calls += o.writev_calls;
        poll_calls += o.poll_calls; poll_empty += o.poll_empty;
        accepts += o.accepts;       closes += o.closes;
        parse_errors += o.parse_errors;
        short_writes += o.short_writes;
        evicted_slow += o.evicted_slow;
        cmd_ring_full += o.cmd_ring_full;
        busy_ns += o.busy_ns; poll_wait_ns += o.poll_wait_ns; spin_polls += o.spin_polls;
    }
    // Snapshot another thread's counters without a data race.
    IoCounters load() const noexcept {
        IoCounters c;
        c.frames_in = load_relaxed(frames_in);       c.frames_out = load_relaxed(frames_out);
        c.bytes_in  = load_relaxed(bytes_in);        c.bytes_out  = load_relaxed(bytes_out);
        c.read_calls = load_relaxed(read_calls);     c.write_calls = load_relaxed(write_calls);
        c.writev_calls = load_relaxed(writev_calls);
        c.poll_calls = load_relaxed(poll_calls);     c.poll_empty = load_relaxed(poll_empty);
        c.accepts = load_relaxed(accepts);           c.closes = load_relaxed(closes);
        c.parse_errors = load_relaxed(parse_errors);
        c.short_writes = load_relaxed(short_writes);
        c.evicted_slow = load_relaxed(evicted_slow);
        c.cmd_ring_full = load_relaxed(cmd_ring_full);
        c.busy_ns = load_relaxed(busy_ns);
        c.poll_wait_ns = load_relaxed(poll_wait_ns);
        c.spin_polls = load_relaxed(spin_polls);
        return c;
    }
};

// Per-shard (simulation thread) counters.
struct GE_CACHE_ALIGNED ShardCounters {
    std::uint64_t ticks           = 0;
    std::uint64_t ticks_late      = 0;   // deadline already passed when the tick began
    std::uint64_t ticks_skipped   = 0;   // fell so far behind that ticks were dropped
    std::uint64_t commands        = 0;
    std::uint64_t entities_stepped= 0;
    std::uint64_t deltas_built    = 0;
    std::uint64_t deltas_dropped  = 0;   // outbound ring full: shed load, do not block
    std::uint64_t aoi_candidates  = 0;
    std::uint64_t kills           = 0;
    std::uint64_t deaths          = 0;
    std::uint64_t sim_ns          = 0;
    std::uint64_t build_ns        = 0;
    std::uint64_t arena_high_water= 0;

    void add(const ShardCounters& o) noexcept {
        ticks += o.ticks; ticks_late += o.ticks_late; ticks_skipped += o.ticks_skipped;
        commands += o.commands; entities_stepped += o.entities_stepped;
        deltas_built += o.deltas_built; deltas_dropped += o.deltas_dropped;
        aoi_candidates += o.aoi_candidates;
        kills += o.kills; deaths += o.deaths;
        sim_ns += o.sim_ns; build_ns += o.build_ns;
        if (o.arena_high_water > arena_high_water) arena_high_water = o.arena_high_water;
    }
    ShardCounters load() const noexcept {
        ShardCounters c;
        c.ticks = load_relaxed(ticks); c.ticks_late = load_relaxed(ticks_late);
        c.ticks_skipped = load_relaxed(ticks_skipped);
        c.commands = load_relaxed(commands);
        c.entities_stepped = load_relaxed(entities_stepped);
        c.deltas_built = load_relaxed(deltas_built);
        c.deltas_dropped = load_relaxed(deltas_dropped);
        c.aoi_candidates = load_relaxed(aoi_candidates);
        c.kills = load_relaxed(kills); c.deaths = load_relaxed(deaths);
        c.sim_ns = load_relaxed(sim_ns); c.build_ns = load_relaxed(build_ns);
        c.arena_high_water = load_relaxed(arena_high_water);
        return c;
    }
};

} // namespace ge::metrics
