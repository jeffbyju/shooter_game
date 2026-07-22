#pragma once
// Process-level resource accounting: memory footprint, CPU utilisation, and the
// scheduler-visible symptoms (context switches, page faults) that explain a
// latency tail when the application-level counters look clean.
#include <cstdint>
#include <string>

#include "ge/core/platform.hpp"

namespace ge::metrics {

struct ProcessSample {
    std::int64_t  wall_ns          = 0;
    std::int64_t  cpu_user_ns      = 0;
    std::int64_t  cpu_system_ns    = 0;
    std::uint64_t rss_bytes        = 0;   // current resident set
    std::uint64_t peak_rss_bytes   = 0;
    std::uint64_t virtual_bytes    = 0;
    std::uint64_t minor_faults     = 0;
    std::uint64_t major_faults     = 0;   // any of these during a run invalidates it
    // Context-switch accounting is not portable in the way it looks.
    //
    // On Linux these mean what they say: voluntary == blocked, involuntary ==
    // preempted, and a high involuntary count is the classic signature of a
    // contended machine.
    //
    // On Darwin they do not. ru_nvcsw is never populated (always 0), and
    // ru_nivcsw tracks blocking-syscall thread transitions rather than
    // preemption -- it scales with poll() rate, not with CPU contention, and a
    // single-threaded spin loop on an idle machine baselines around 700/s while
    // an idle-but-polling server reports six figures. Reported here for
    // completeness, but scheduler interference is inferred from tick jitter
    // instead, which is measured by this program and means the same thing
    // everywhere. See ge::bench::print_validity.
    std::uint64_t vol_ctx_switches   = 0;
    std::uint64_t invol_ctx_switches = 0;
    std::uint32_t thread_count     = 0;

    std::int64_t cpu_ns() const noexcept { return cpu_user_ns + cpu_system_ns; }
};

ProcessSample sample_process() noexcept;

// Difference of two samples, presented the way a capacity plan needs it.
struct ResourceDelta {
    std::int64_t  wall_ns            = 0;
    std::int64_t  cpu_ns             = 0;
    std::uint64_t minor_faults       = 0;
    std::uint64_t major_faults       = 0;
    std::uint64_t vol_ctx_switches   = 0;
    std::uint64_t invol_ctx_switches = 0;
    std::uint64_t rss_bytes          = 0;
    std::uint64_t peak_rss_bytes     = 0;

    // Cores fully saturated, e.g. 2.4 means 2.4 cores' worth of CPU was burned.
    double cores_used() const noexcept {
        return wall_ns > 0 ? double(cpu_ns) / double(wall_ns) : 0.0;
    }
    // Utilisation as a percentage of the machine.
    double utilisation_pct(unsigned cores) const noexcept {
        return cores ? cores_used() / double(cores) * 100.0 : 0.0;
    }
    // The number that actually sizes a fleet: CPU-microseconds per message.
    double cpu_us_per_message(std::uint64_t messages) const noexcept {
        return messages ? double(cpu_ns) / 1000.0 / double(messages) : 0.0;
    }
};

ResourceDelta diff(const ProcessSample& before, const ProcessSample& after) noexcept;

// Per-thread CPU accounting, so a report can say *which* thread saturated.
struct ThreadCpu {
    std::string  name;
    std::int64_t cpu_ns  = 0;
    std::int64_t wall_ns = 0;
    double utilisation() const noexcept { return wall_ns ? double(cpu_ns) / double(wall_ns) : 0.0; }
};

std::string format_bytes(std::uint64_t bytes);

} // namespace ge::metrics
