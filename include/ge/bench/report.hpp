#pragma once
// Report rendering.
//
// A benchmark that prints a single number is not a benchmark. Every figure here
// is printed with the context needed to judge it: the sample count behind a
// percentile, the clock resolution the latency was measured against, whether
// the load generator itself was saturated, and whether the run took a page
// fault or an involuntary context switch that would invalidate it.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ge/bench/loadgen.hpp"
#include "ge/metrics/histogram.hpp"
#include "ge/metrics/perf_counters.hpp"
#include "ge/metrics/process.hpp"
#include "ge/server/world_server.hpp"

namespace ge::bench {

struct RunEnvironment {
    std::string  host_arch;
    std::string  os;
    std::string  compiler;
    std::string  build_type;
    unsigned     logical_cores      = 0;
    unsigned     performance_cores  = 0;
    std::int64_t clock_overhead_ns  = 0;
    std::int64_t clock_granularity_ns = 0;
    bool         alloc_probe        = false;
    bool         threads_pinned     = false;
    std::string  topology;
    std::string  perf_counters;      // availability, or why not
    bool         reuseport          = false;
};

RunEnvironment probe_environment();
void print_environment(const RunEnvironment& env);

// One latency distribution as a row block. `note` explains what the number
// means end to end, because "p99 = 180us" is meaningless without the path.
void print_latency(const char* label, const metrics::LatencySummary& s, const char* note);

void print_throughput(const LoadReport& load, const server::ServerReport& srv,
                      bool server_counters_available);
// Always available, because it comes from the generator rather than the server.
void print_client_observations(const LoadReport& load);
void print_allocations(const LoadReport& load, const server::ServerReport& srv);
void print_memory(const server::ServerReport& srv, const metrics::ProcessSample& proc,
                  std::uint64_t live_connections);
void print_cpu(const metrics::ResourceDelta& server_res, const LoadReport& load,
               const server::ServerReport& srv, const RunEnvironment& env);
void print_syscalls(const server::ServerReport& srv);
// Hardware counters for the measured window. Prints why they are missing rather
// than silently omitting the section, because "no cache data" and "cache data
// is fine" look identical otherwise.
void print_hardware_counters(const metrics::PerfSample& s, std::uint64_t messages);
void print_backpressure(const server::ServerReport& srv, const LoadReport& load);
// `tick_hz` and the server report supply the portable contention signal: a
// fixed-rate simulation loop that starts late is direct evidence the scheduler
// is not delivering its thread on time.
void print_validity(const metrics::ResourceDelta& server_res, const LoadReport& load,
                    const RunEnvironment& env, const server::ServerReport& srv,
                    int tick_hz, bool have_server);

// Dumps full latency distributions as CSV: one row per occupied histogram
// bucket, with the cumulative fraction, so a reader can plot the CDF rather than
// squinting at seven percentiles.
bool write_histogram_csv(const std::string& path,
                         const std::vector<std::pair<std::string, const metrics::Histogram*>>& series);

// Machine-readable sibling of the console output, for plotting and CI diffs.
bool write_json(const std::string& path, const RunEnvironment& env, const LoadConfig& cfg,
                const LoadReport& load, const server::ServerReport& srv,
                const metrics::ResourceDelta& server_res);

// One row of the offered-load sweep.
struct SweepPoint {
    double        offered_msgs_per_s  = 0;
    double        achieved_msgs_per_s = 0;
    std::uint64_t p50 = 0, p99 = 0, p999 = 0, max = 0;
    double        cores_used = 0;
    std::uint64_t drops = 0;
    // Set when the generator itself ran out of CPU. A point flagged here
    // measures the client, not the server, and must not be read as capacity.
    bool          client_limited = false;
    double        fidelity_pct = 0;   // fraction of the offered schedule delivered
};
void print_sweep(const std::vector<SweepPoint>& points);
bool write_sweep_csv(const std::string& path, const std::vector<SweepPoint>& points);

} // namespace ge::bench
