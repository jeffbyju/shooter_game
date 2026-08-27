#pragma once
// Hot-path counters.
//
// Each thread increments its own counter -- no atomic RMW, no contention, one
// store per event. Aggregation happens only when a report is produced. The cost
// of this design is that a snapshot can miss an in-flight increment; at the
// rates these count that is a rounding error, and it is the right trade for
// keeping the measured path identical to the unmeasured one.
//
// These used to be plain uint64 written by the owning thread and read through
// std::atomic_ref, with a comment claiming that made the cross-thread read
// "well-defined rather than a data race we agreed not to talk about".
//
// That was wrong, and ThreadSanitizer said so on every counter in the struct.
// A data race needs BOTH accesses to be atomic; one atomic load against one
// plain store is still a race and still undefined. The practical hazard is not
// tearing -- an aligned 64-bit store is atomic on every machine this targets --
// it is that the compiler may legally keep a plain counter in a register across
// a loop and write it back once at the end, leaving a reader staring at a
// frozen value for as long as the loop runs. Which is exactly the situation a
// per-second stats line is designed to detect.
#include <atomic>
#include <cstdint>

#include "ge/core/platform.hpp"

namespace ge::metrics {

// A counter written by exactly one owning thread and read by any other.
//
// Relaxed load + relaxed store, NOT fetch_add. That distinction is the whole
// design: because a single thread owns the increment, the read-modify-write
// does not need to be atomic, only the individual accesses do. Checked in the
// assembly rather than assumed -- on arm64 at -O2:
//
//     plain ++x                     ldr / add / str
//     relaxed load + relaxed store  ldr / add / str      <- identical
//     fetch_add(relaxed)            ldadd                <- a real RMW
//     fetch_add(seq_cst)            ldaddal              <- RMW plus barriers
//
// So defined behaviour here costs nothing, and the counter stays a plain store
// on the hot path exactly as the original design intended.
class Counter {
public:
    Counter() noexcept = default;
    Counter(std::uint64_t v) noexcept : v_(v) {}
    Counter(const Counter& o) noexcept : v_(o.get()) {}
    Counter& operator=(const Counter& o) noexcept { set(o.get()); return *this; }
    Counter& operator=(std::uint64_t v) noexcept  { set(v); return *this; }

    operator std::uint64_t() const noexcept { return get(); }
    std::uint64_t get() const noexcept { return v_.load(std::memory_order_relaxed); }
    void set(std::uint64_t v) noexcept { v_.store(v, std::memory_order_relaxed); }

    Counter&      operator++()    noexcept { set(get() + 1); return *this; }
    std::uint64_t operator++(int) noexcept { const auto t = get(); set(t + 1); return t; }
    Counter& operator+=(std::uint64_t n) noexcept { set(get() + n); return *this; }

private:
    std::atomic<std::uint64_t> v_{0};
};

// Per-I/O-thread counters. Syscall counts are tracked separately from message
// counts because messages-per-syscall is the batching efficiency number, and
// it is the first thing to check when throughput stops scaling.
struct GE_CACHE_ALIGNED IoCounters {
    Counter      frames_in       = 0;
    Counter      frames_out      = 0;
    Counter      bytes_in        = 0;
    Counter      bytes_out       = 0;
    Counter      read_calls      = 0;
    Counter      write_calls     = 0;
    Counter      writev_calls    = 0;
    Counter      poll_calls      = 0;
    Counter      poll_empty      = 0;   // wakeups that found no work
    Counter      accepts         = 0;
    Counter      closes          = 0;
    Counter      parse_errors    = 0;
    Counter      short_writes    = 0;   // kernel buffer full: backpressure begins here
    Counter      evicted_slow    = 0;   // clients dropped for not draining
    Counter      cmd_ring_full   = 0;   // I/O -> shard backpressure
    Counter      busy_ns         = 0;   // CPU actually spent on work
    Counter      poll_wait_ns    = 0;   // blocked inside poll() with nothing to do
    Counter      spin_polls      = 0;   // zero-timeout polls issued while spinning

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
    // Snapshot another thread's counters. Every field reads itself atomically
    // now, so this is just the copy constructor doing the right thing.
    IoCounters load() const noexcept { return *this; }
};

// Per-shard (simulation thread) counters.
struct GE_CACHE_ALIGNED ShardCounters {
    Counter      ticks           = 0;
    Counter      ticks_late      = 0;   // deadline already passed when the tick began
    Counter      ticks_skipped   = 0;   // fell so far behind that ticks were dropped
    Counter      commands        = 0;
    Counter      entities_stepped= 0;
    Counter      deltas_built    = 0;
    Counter      deltas_dropped  = 0;   // outbound ring full: shed load, do not block
    Counter      aoi_candidates  = 0;
    Counter      kills           = 0;
    Counter      deaths          = 0;
    Counter      sim_ns          = 0;
    Counter      build_ns        = 0;
    Counter      arena_high_water= 0;

    void add(const ShardCounters& o) noexcept {
        ticks += o.ticks; ticks_late += o.ticks_late; ticks_skipped += o.ticks_skipped;
        commands += o.commands; entities_stepped += o.entities_stepped;
        deltas_built += o.deltas_built; deltas_dropped += o.deltas_dropped;
        aoi_candidates += o.aoi_candidates;
        kills += o.kills; deaths += o.deaths;
        sim_ns += o.sim_ns; build_ns += o.build_ns;
        if (o.arena_high_water > arena_high_water) arena_high_water = o.arena_high_water;
    }
    ShardCounters load() const noexcept { return *this; }
};

} // namespace ge::metrics
