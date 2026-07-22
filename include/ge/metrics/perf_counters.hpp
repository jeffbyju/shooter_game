#pragma once
// Hardware performance counters.
//
// Everything else in this project measures *time*. Time tells you something is
// slow; it does not tell you why. The CPU itself counts the things that explain
// it -- how many instructions actually retired, how often a memory access
// missed cache, how often a branch was predicted wrong -- and those counters are
// the difference between "this loop is slow" and "this loop is memory-bound".
//
// Three derived numbers carry most of the value:
//
//   IPC (instructions per cycle)
//       Modern cores issue 4-8 instructions per cycle. Measuring 0.5 means the
//       core is stalled 90% of the time -- almost always waiting for memory.
//       Measuring 3+ means you are actually compute-bound and the only way
//       forward is fewer instructions.
//
//   cache misses per operation
//       At ~100ns per miss to main memory, one miss per message at 100k msg/s
//       is 10ms/s of pure stall. This is the number that justifies every layout
//       decision in the project -- structure-of-arrays, cache-line padding,
//       the CSR spatial grid.
//
//   branch mispredictions per operation
//       ~15-20 cycles each. Tells you whether a data-dependent branch is being
//       predicted well (rare, cheap) or badly (hot, expensive).
//
// Availability: Linux only, via perf_event_open(2). Requires
// kernel.perf_event_paranoid <= 2 for user-space counting; cloud VMs often
// default to 2 or 3, and virtualised PMUs are sometimes absent entirely. macOS
// has no unprivileged equivalent -- kperf is private API and requires
// entitlements -- so this reports "unavailable" there rather than inventing
// numbers.
#include <cstdint>
#include <string>

#include "ge/core/platform.hpp"

namespace ge::metrics {

struct PerfSample {
    std::uint64_t cycles             = 0;
    std::uint64_t instructions       = 0;
    std::uint64_t cache_references   = 0;
    std::uint64_t cache_misses       = 0;
    std::uint64_t branches           = 0;
    std::uint64_t branch_misses      = 0;
    bool          valid              = false;

    double ipc() const noexcept {
        return cycles ? double(instructions) / double(cycles) : 0.0;
    }
    double cache_miss_rate() const noexcept {
        return cache_references ? double(cache_misses) / double(cache_references) : 0.0;
    }
    double branch_miss_rate() const noexcept {
        return branches ? double(branch_misses) / double(branches) : 0.0;
    }
    PerfSample operator-(const PerfSample& o) const noexcept {
        return {cycles - o.cycles, instructions - o.instructions,
                cache_references - o.cache_references, cache_misses - o.cache_misses,
                branches - o.branches, branch_misses - o.branch_misses,
                valid && o.valid};
    }
};

// Opens a group of counters for the calling process on every CPU it runs on.
// Construction never throws; if the counters cannot be opened -- wrong kernel,
// paranoid setting, no PMU in the VM -- available() is false and the reason is
// in unavailable_reason().
class PerfCounters {
public:
    PerfCounters();
    ~PerfCounters();
    PerfCounters(const PerfCounters&)            = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    bool        available() const noexcept { return available_; }
    const std::string& unavailable_reason() const noexcept { return reason_; }

    void start() noexcept;          // reset and enable
    void stop()  noexcept;          // disable; counts are frozen
    PerfSample read() const noexcept;

private:
    static constexpr int kCount = 6;
    int         fds_[kCount];
    bool        available_ = false;
    std::string reason_;
};

// Scopes a measurement. delta() is the counter activity inside it.
class PerfScope {
public:
    explicit PerfScope(PerfCounters& c) noexcept : c_(c) { c_.start(); }
    PerfSample delta() const noexcept { return c_.read(); }
private:
    PerfCounters& c_;
};

std::string describe_perf_availability();

} // namespace ge::metrics
