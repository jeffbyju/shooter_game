#pragma once
// Open-loop load generator.
//
// The measurement methodology is the point of this file, so it is stated
// explicitly:
//
// 1. OPEN LOOP. Send times come from a schedule fixed before the run starts
//    (origin + k*interval), not from "previous reply received". A closed-loop
//    client throttles itself whenever the server slows down, so the server is
//    never actually offered the load it is being credited with, and the tail
//    it reports is the tail of a load it never saw.
//
// 2. COORDINATED-OMISSION-FREE LATENCY. Latency is measured from the time a
//    request was *scheduled* to be sent, not from the time it actually went
//    out. If the client is late issuing a request -- which is exactly what
//    happens when the server stalls -- that lateness is part of what a real
//    user experiences and is counted. Both figures are reported: `scheduled`
//    (the honest one) and `wire` (send-to-receive), because the gap between
//    them is itself a diagnostic.
//
// 3. WARMUP IS DISCARDED. Page faults, branch predictor training, TCP window
//    growth and CPU frequency ramp all happen in the first second. Histograms
//    are reset after the warmup window so the reported distribution is steady
//    state.
//
// 4. THE INSTRUMENT IS MEASURED TOO. The generator reports its own CPU use and
//    allocation count. If the client saturates its cores, the numbers describe
//    the client, and the report says so rather than quietly attributing it to
//    the server.
#include <cstdint>
#include <string>
#include <vector>

#include "ge/alloc/counters.hpp"
#include "ge/core/error.hpp"
#include "ge/metrics/histogram.hpp"
#include "ge/metrics/process.hpp"

namespace ge::bench {

struct LoadConfig {
    std::string   host          = "127.0.0.1";
    std::uint16_t port          = 9000;
    std::uint32_t clients       = 100;
    std::uint32_t threads       = 2;
    double        ping_rate_hz  = 50.0;   // per client
    double        move_rate_hz  = 30.0;   // per client
    double        duration_s    = 10.0;
    double        warmup_s      = 2.0;
    bool          poisson       = true;   // exponential gaps; bursty like real traffic
    std::uint64_t seed          = 0x5EED;
    int           poll_timeout_us = 200;
    double        connect_timeout_s = 10.0;
    // When set, the merged client-side latency distributions are written here as
    // CSV (one row per occupied histogram bucket) for plotting.
    std::string   histogram_csv_path;
};

struct LoadReport {
    metrics::LatencySummary ping_scheduled{};  // scheduled-send -> reply (the headline)
    metrics::LatencySummary ping_wire{};       // actual-send -> reply
    metrics::LatencySummary e2e_scheduled{};   // input -> the tick that reflects it
    metrics::LatencySummary send_delay{};      // how late the generator itself was
    metrics::LatencySummary upstream{};        // client send -> server receive
    metrics::LatencySummary downstream{};      // server receive -> client receive

    std::uint64_t clients_connected = 0;
    std::uint64_t pings_sent        = 0;
    std::uint64_t pongs_received    = 0;
    std::uint64_t moves_sent        = 0;
    std::uint64_t deltas_received   = 0;
    std::uint64_t entities_received = 0;
    std::uint64_t bytes_sent        = 0;
    std::uint64_t bytes_received    = 0;
    std::uint64_t parse_errors      = 0;
    std::uint64_t disconnects       = 0;

    double  measured_seconds  = 0;
    double  offered_msgs_per_s  = 0;   // what the schedule demanded
    double  achieved_msgs_per_s = 0;   // what actually went on the wire
    double  reply_msgs_per_s    = 0;   // what came back

    metrics::ResourceDelta client_resources{};
    alloc::AllocStats      client_alloc_steady{};
    bool                   client_saturated = false;  // generator CPU >= 90% of its threads
};

// Blocks for roughly warmup_s + duration_s. Returns the report, or the reason
// the run could not be made -- a run that failed to connect has no numbers worth
// reporting, and returning an empty report plus a bool invites printing it.
core::Result<LoadReport> run_load(const LoadConfig& cfg);

} // namespace ge::bench
