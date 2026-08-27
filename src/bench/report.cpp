#include "ge/bench/report.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "ge/alloc/counters.hpp"
#include "ge/net/socket.hpp"
#include "ge/core/clock.hpp"
#include "ge/core/thread.hpp"
#include "ge/core/topology.hpp"

namespace ge::bench {
namespace {

// Latencies span ns to seconds; a fixed unit makes one end unreadable.
std::string dur(std::uint64_t ns) {
    char b[32];
    if (ns < 1'000)            std::snprintf(b, sizeof b, "%" PRIu64 " ns", ns);
    else if (ns < 1'000'000)   std::snprintf(b, sizeof b, "%.2f us", double(ns) / 1e3);
    else if (ns < 1'000'000'000) std::snprintf(b, sizeof b, "%.2f ms", double(ns) / 1e6);
    else                       std::snprintf(b, sizeof b, "%.3f s",  double(ns) / 1e9);
    return b;
}

std::string count(double v) {
    char b[32];
    if (v < 1e3)      std::snprintf(b, sizeof b, "%.0f", v);
    else if (v < 1e6) std::snprintf(b, sizeof b, "%.2f K", v / 1e3);
    else              std::snprintf(b, sizeof b, "%.3f M", v / 1e6);
    return b;
}

void rule(const char* title) {
    std::printf("\n\033[1m%s\033[0m\n", title);
    std::printf("--------------------------------------------------------------------------\n");
}

double safe_div(double a, double b) { return b != 0 ? a / b : 0.0; }

} // namespace

RunEnvironment probe_environment() {
    RunEnvironment e;
#if defined(GE_ARCH_ARM64)
    e.host_arch = "arm64";
#elif defined(GE_ARCH_X86_64)
    e.host_arch = "x86_64";
#else
    e.host_arch = "unknown";
#endif
#if defined(GE_OS_MACOS)
    e.os = "macOS";
#else
    e.os = "Linux";
#endif
    char buf[64];
    // Compiler identification has to be compiler-agnostic, which sounds obvious
    // and was not: the clang-only version macros built fine on macOS and broke
    // the first GCC build. Check for clang first -- it also defines __GNUC__.
#if defined(__clang__)
    std::snprintf(buf, sizeof buf, "clang %d.%d.%d",
                  __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    std::snprintf(buf, sizeof buf, "gcc %d.%d.%d",
                  __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    std::snprintf(buf, sizeof buf, "unknown compiler");
#endif
    e.compiler = buf;
#ifdef NDEBUG
    e.build_type = "optimised (-O3, NDEBUG)";
#else
    e.build_type = "debug (assertions on -- latencies are NOT representative)";
#endif
    e.logical_cores     = core::hardware_cores();
    e.performance_cores = core::performance_cores();
    const auto p = core::Clock::probe();
    e.clock_overhead_ns    = p.overhead_ns;
    e.clock_granularity_ns = p.granularity_ns;
    e.alloc_probe   = alloc::probe_enabled();
    e.topology      = core::topology().describe();
    e.perf_counters = metrics::describe_perf_availability();
    e.reuseport     = net::reuseport_balances_connections();
    return e;
}

void print_environment(const RunEnvironment& e) {
    rule("ENVIRONMENT");
    std::printf("  platform              %s / %s\n", e.host_arch.c_str(), e.os.c_str());
    std::printf("  compiler              %s\n", e.compiler.c_str());
    std::printf("  build                 %s\n", e.build_type.c_str());
    std::printf("  cores                 %u logical, %u performance\n", e.logical_cores, e.performance_cores);
    std::printf("  topology              %s\n", e.topology.c_str());
    std::printf("  hardware counters     %s\n", e.perf_counters.c_str());
    std::printf("  SO_REUSEPORT          %s\n",
                e.reuseport ? "balances connections (per-thread listeners available)"
                            : "not load-balancing on this platform; using an acceptor thread");
    std::printf("  thread pinning        %s\n",
                e.threads_pinned ? "yes (sched_setaffinity)"
                                 : "NO -- unavailable on this platform; expect wider tails");
    std::printf("  clock call overhead   %s   (subtracted from nothing; it is inside every sample)\n",
                dur(std::uint64_t(e.clock_overhead_ns)).c_str());
    std::printf("  clock granularity     %s   (any latency near this is instrument noise)\n",
                dur(std::uint64_t(e.clock_granularity_ns)).c_str());
    std::printf("  allocation probe      %s\n", e.alloc_probe ? "active (global new/delete interposed)"
                                                              : "DISABLED -- alloc counts unmeasured");
}

void print_latency(const char* label, const metrics::LatencySummary& s, const char* note) {
    std::printf("  %-22s n=%-10s min %-10s p50 %-10s p90 %-10s p95 %-10s\n",
                label, count(double(s.count)).c_str(), dur(s.min).c_str(),
                dur(s.p50).c_str(), dur(s.p90).c_str(), dur(s.p95).c_str());
    std::printf("  %-22s %-12s p99 %-10s p99.9 %-8s p99.99 %-8s max %-10s\n",
                "", "", dur(s.p99).c_str(), dur(s.p999).c_str(),
                dur(s.p9999).c_str(), dur(s.max).c_str());
    std::printf("  %-22s mean %-9s stddev %-9s\n", "",
                dur(std::uint64_t(s.mean)).c_str(), dur(std::uint64_t(s.stddev)).c_str());
    if (note && *note) std::printf("  %-22s \033[2m%s\033[0m\n", "", note);
    std::printf("\n");
}

void print_throughput(const LoadReport& load, const server::ServerReport& srv, bool have_server) {
    rule("THROUGHPUT  (messages per second)");
    const double total_msgs = double(load.pings_sent + load.moves_sent +
                                     load.pongs_received + load.deltas_received);
    std::printf("  offered (schedule)    %12s msg/s\n", count(load.offered_msgs_per_s).c_str());
    std::printf("  client -> server      %12s msg/s   (%s frames over %.1fs)\n",
                count(load.achieved_msgs_per_s).c_str(),
                count(double(load.pings_sent + load.moves_sent)).c_str(), load.measured_seconds);
    std::printf("  server -> client      %12s msg/s   (%s pongs + %s state deltas)\n",
                count(load.reply_msgs_per_s).c_str(),
                count(double(load.pongs_received)).c_str(),
                count(double(load.deltas_received)).c_str());
    std::printf("  total wire messages   %12s msg/s\n", count(total_msgs / load.measured_seconds).c_str());
    std::printf("  entity snapshots      %12s /s      (payload the deltas actually carried)\n",
                count(double(load.entities_received) / load.measured_seconds).c_str());
    std::printf("  bytes                 %12s in/s  %s out/s\n",
                count(double(load.bytes_received) / load.measured_seconds).c_str(),
                count(double(load.bytes_sent) / load.measured_seconds).c_str());
    if (have_server && !srv.io_backend.empty())
        std::printf("  io backend            %12s\n", srv.io_backend.c_str());
    if (have_server) {
        std::printf("  server frames handled %12s in    %s out\n",
                    count(double(std::uint64_t(srv.io.frames_in))).c_str(), count(double(std::uint64_t(srv.io.frames_out))).c_str());
        std::printf("  simulation            %12s ticks  %s entity-steps\n",
                    count(double(std::uint64_t(srv.shard.ticks))).c_str(),
                    count(double(std::uint64_t(srv.shard.entities_stepped))).c_str());
    } else {
        std::printf("  server-side counters       unavailable  (measuring an external process)\n");
    }
    const double delivered = safe_div(load.achieved_msgs_per_s, load.offered_msgs_per_s) * 100.0;
    std::printf("  schedule fidelity     %11.2f%%       %s\n", delivered,
                delivered >= 99.0 ? "(generator kept up: offered load is real)"
                                  : "(generator fell behind: offered load was NOT delivered)");
}

void print_client_observations(const LoadReport& load) {
    rule("CLIENT-SIDE OBSERVATIONS  (what the generator saw, no server access needed)");
    std::printf("  clients connected     %12" PRIu64 "\n", load.clients_connected);
    std::printf("  connections lost      %12" PRIu64 "   server closed or reset mid-run\n", load.disconnects);
    std::printf("  parse errors          %12" PRIu64 "   malformed frame from the server\n", std::uint64_t(load.parse_errors));
    std::printf("  pings sent / answered %12" PRIu64 " / %" PRIu64 "   (%.3f%% answered)\n",
                load.pings_sent, load.pongs_received,
                load.pings_sent ? 100.0 * double(load.pongs_received) / double(load.pings_sent) : 0.0);
    std::printf("  inputs sent           %12" PRIu64 "   state deltas received %" PRIu64 "\n",
                load.moves_sent, load.deltas_received);
}

void print_allocations(const LoadReport& load, const server::ServerReport& srv) {
    rule("ALLOCATIONS  (the steady-state target is exactly zero)");
    if (!alloc::probe_enabled()) {
        std::printf("  allocation probe disabled; rebuild with -DGE_ALLOC_PROBE=ON\n");
        return;
    }
    const auto& d = load.client_alloc_steady;
    const double msgs = double(load.pings_sent + load.moves_sent + load.pongs_received + load.deltas_received);
    std::printf("  measured window       steady state only (warmup and connect excluded)\n");
    std::printf("  allocations           %12" PRIu64 "    frees %" PRIu64 "\n", d.allocs, d.frees);
    std::printf("  bytes allocated       %12s    freed %s\n",
                metrics::format_bytes(d.bytes_allocated).c_str(),
                metrics::format_bytes(d.bytes_freed).c_str());
    std::printf("  allocations / 1M msgs %12.2f\n", msgs > 0 ? double(d.allocs) * 1e6 / msgs : 0.0);
    std::printf("  process lifetime      %12" PRIu64 " allocations total (startup: rings, arenas, pools)\n",
                srv.alloc_total.allocs);
    if (d.allocs == 0)
        std::printf("  \033[32mverdict               ZERO allocations on the hot path\033[0m\n");
    else
        std::printf("  \033[33mverdict               %" PRIu64 " allocations leaked into the hot path\033[0m\n", d.allocs);
    std::printf("  \033[2mnote                  counts are process-wide and include the load\n"
                "                        generator; the server and client share this process\n"
                "                        in --inproc mode, so this is an upper bound on the\n"
                "                        server's own allocation count.\033[0m\n");
}

void print_memory(const server::ServerReport& srv, const metrics::ProcessSample& proc,
                  std::uint64_t live) {
    rule("MEMORY FOOTPRINT");
    std::printf("  resident (RSS)        %12s\n", metrics::format_bytes(proc.rss_bytes).c_str());
    std::printf("  peak resident         %12s\n", metrics::format_bytes(proc.peak_rss_bytes).c_str());
    std::printf("  virtual               %12s   (reservations; not all resident)\n",
                metrics::format_bytes(proc.virtual_bytes).c_str());
    std::printf("  reserved up front     %12s   (rings, arenas, pools, histograms)\n",
                metrics::format_bytes(srv.reserved_bytes).c_str());
    std::printf("  per connection        %12s   (read buf + write buf + connection object)\n",
                metrics::format_bytes(srv.bytes_per_conn).c_str());
    if (live)
        std::printf("  RSS per live conn     %12s   over %" PRIu64 " connections\n",
                    metrics::format_bytes(proc.rss_bytes / live).c_str(), live);
    std::printf("  threads               %12u\n", proc.thread_count);
    std::printf("  \033[2mnote                  footprint is bounded at startup: buffers, queues and\n"
                "                        entity storage are all fixed-capacity, so RSS does not\n"
                "                        track load once the connection count is steady.\033[0m\n");
}

void print_cpu(const metrics::ResourceDelta& res, const LoadReport& load,
               const server::ServerReport& srv, const RunEnvironment& env) {
    rule("CPU UTILISATION");
    const double msgs = double(load.pings_sent + load.moves_sent + load.pongs_received + load.deltas_received);
    std::printf("  wall time             %12s\n", dur(std::uint64_t(res.wall_ns)).c_str());
    std::printf("  cpu time              %12s\n", dur(std::uint64_t(res.cpu_ns)).c_str());
    std::printf("  cores saturated       %12.3f   of %u logical (%.1f%% of the machine)\n",
                res.cores_used(), env.logical_cores, res.utilisation_pct(env.logical_cores));
    std::printf("  cpu per message       %12.3f us  <- the number that sizes a fleet\n",
                res.cpu_us_per_message(std::uint64_t(msgs)));
    std::printf("  messages per core-sec %12s\n",
                count(res.cpu_ns > 0 ? msgs / (double(res.cpu_ns) / 1e9) : 0).c_str());

    const std::uint64_t busy = std::uint64_t(srv.io.busy_ns), wait = std::uint64_t(srv.io.poll_wait_ns);
    if (busy + wait > 0)
        std::printf("  io threads busy       %11.2f%%       (%s working / %s blocked in poll)\n",
                    100.0 * double(busy) / double(busy + wait),
                    dur(busy).c_str(), dur(wait).c_str());
    if (std::uint64_t(srv.io.spin_polls) > 0)
        std::printf("  adaptive spin         %12" PRIu64 " zero-timeout polls of %" PRIu64
                    " (%.1f%%): trading CPU for wakeup latency\n",
                    std::uint64_t(srv.io.spin_polls), std::uint64_t(srv.io.poll_calls),
                    100.0 * double(std::uint64_t(srv.io.spin_polls)) / double(std::uint64_t(srv.io.poll_calls)));
    if (std::uint64_t(srv.shard.ticks) > 0) {
        const double per_tick = double(std::uint64_t(srv.shard.sim_ns) + std::uint64_t(srv.shard.build_ns)) / double(std::uint64_t(srv.shard.ticks));
        std::printf("  shard tick budget     %11.2f%%       (%s of work in each tick)\n",
                    100.0 * per_tick / (1e9 / 60.0), dur(std::uint64_t(per_tick)).c_str());
        std::printf("    integrate + grid    %12s /tick\n",
                    dur(std::uint64_t(srv.shard.sim_ns) / std::uint64_t(srv.shard.ticks)).c_str());
        std::printf("    build deltas        %12s /tick\n",
                    dur(std::uint64_t(srv.shard.build_ns) / std::uint64_t(srv.shard.ticks)).c_str());
    }
#if defined(GE_OS_MACOS)
    std::printf("  context switches      %12" PRIu64 " (Darwin: blocking-syscall transitions,\n"
                "                                     not preemptions; voluntary is never populated)\n",
                res.invol_ctx_switches);
#else
    std::printf("  context switches      %12" PRIu64 " voluntary, %" PRIu64 " involuntary\n",
                res.vol_ctx_switches, res.invol_ctx_switches);
#endif
    std::printf("  page faults           %12" PRIu64 " minor, %" PRIu64 " major\n",
                res.minor_faults, res.major_faults);
}

void print_hardware_counters(const metrics::PerfSample& s, std::uint64_t messages) {
    rule("HARDWARE COUNTERS  (why it is slow, not just that it is)");
    if (!s.valid) {
        std::printf("  %s\n", metrics::describe_perf_availability().c_str());
        std::printf("  \033[2mWithout these, every claim about cache behaviour in this project\n"
                    "  rests on timing alone. Run on Linux with\n"
                    "  kernel.perf_event_paranoid <= 2 to get them.\033[0m\n");
        return;
    }
    const double msgs = double(messages ? messages : 1);
    std::printf("  instructions          %16s\n", count(double(s.instructions)).c_str());
    std::printf("  cycles                %16s\n", count(double(s.cycles)).c_str());
    std::printf("  IPC                   %16.3f   %s\n", s.ipc(),
                s.ipc() < 1.0 ? "<- under 1.0: the core is stalled, almost always on memory"
                              : "(a core can issue 4-8; higher means compute-bound)");
    std::printf("  cache references      %16s\n", count(double(s.cache_references)).c_str());
    std::printf("  cache misses          %16s   %.2f%% of references\n",
                count(double(s.cache_misses)).c_str(), s.cache_miss_rate() * 100.0);
    std::printf("  branch misses         %16s   %.2f%% of branches\n",
                count(double(s.branch_misses)).c_str(), s.branch_miss_rate() * 100.0);
    std::printf("\n");
    std::printf("  per message: %.1f instructions, %.1f cycles, %.3f cache misses, "
                "%.3f branch misses\n",
                double(s.instructions) / msgs, double(s.cycles) / msgs,
                double(s.cache_misses) / msgs, double(s.branch_misses) / msgs);
    std::printf("  \033[2mCache misses per message is the number that justifies the data layout\n"
                "  decisions: structure-of-arrays, 128-byte padding, the CSR spatial grid.\n"
                "  At ~100ns per miss, one per message at 100k msg/s is 10ms/s of stall.\033[0m\n");
}

void print_syscalls(const server::ServerReport& srv) {
    rule("SYSCALL EFFICIENCY  (batching quality)");
    const double reads = double(std::uint64_t(srv.io.read_calls)), writes = double(std::uint64_t(srv.io.write_calls));
    std::printf("  frames per read()     %12.2f\n", safe_div(double(std::uint64_t(srv.io.frames_in)), reads));
    std::printf("  frames per write()    %12.2f\n", safe_div(double(std::uint64_t(srv.io.frames_out)), writes));
    std::printf("  bytes per read()      %12.1f\n", safe_div(double(std::uint64_t(srv.io.bytes_in)), reads));
    std::printf("  events per poll()     %12.2f   (%" PRIu64 " polls, %" PRIu64 " empty wakeups)\n",
                safe_div(double(std::uint64_t(srv.io.frames_in)), double(std::uint64_t(srv.io.poll_calls))),
                std::uint64_t(srv.io.poll_calls), std::uint64_t(srv.io.poll_empty));
    std::printf("  writev batches        %12" PRIu64 "\n", std::uint64_t(srv.io.writev_calls));
    std::printf("  \033[2mnote                  an empty wakeup is a poll() that found no work; a high\n"
                "                        ratio means the timeout is too short or something is\n"
                "                        waking the loop without producing anything.\033[0m\n");
}

void print_backpressure(const server::ServerReport& srv, const LoadReport& load) {
    rule("BACKPRESSURE AND LOSS  (every drop is counted, none are silent)");
    std::printf("  command ring full     %12" PRIu64 "   inputs dropped before reaching a shard\n", std::uint64_t(srv.io.cmd_ring_full));
    std::printf("  state deltas dropped  %12" PRIu64 "   shard could not enqueue (I/O thread behind)\n", std::uint64_t(srv.shard.deltas_dropped));
    std::printf("  short writes          %12" PRIu64 "   kernel send buffer full; output queued\n", std::uint64_t(srv.io.short_writes));
    std::printf("  slow clients evicted  %12" PRIu64 "   send queue overflowed its fixed budget\n", std::uint64_t(srv.io.evicted_slow));
    std::printf("  parse errors          %12" PRIu64 "   malformed frames; connection torn down\n", std::uint64_t(srv.io.parse_errors));
    std::printf("  ticks late            %12" PRIu64 "   tick began after its deadline\n", std::uint64_t(srv.shard.ticks_late));
    std::printf("  ticks skipped         %12" PRIu64 "   dropped rather than replayed to catch up\n", std::uint64_t(srv.shard.ticks_skipped));
    std::printf("  client disconnects    %12" PRIu64 "   observed by the load generator\n", load.disconnects);
    std::printf("  client send blocked   %12" PRIu64 "   generator could not issue on schedule\n", std::uint64_t(load.parse_errors));
    const double drop_rate = safe_div(double(std::uint64_t(srv.shard.deltas_dropped)),
                                      double(std::uint64_t(srv.shard.deltas_built) + std::uint64_t(srv.shard.deltas_dropped))) * 100.0;
    std::printf("  delta drop rate       %11.4f%%\n", drop_rate);
}

void print_validity(const metrics::ResourceDelta& res, const LoadReport& load,
                    const RunEnvironment& env, const server::ServerReport& srv,
                    int tick_hz, bool have_server) {
    rule("RUN VALIDITY  (reasons to distrust the numbers above)");
    bool clean = true;
    auto flag = [&](bool bad, const char* msg) {
        if (bad) { std::printf("  \033[33m[warn]\033[0m %s\n", msg); clean = false; }
    };
    flag(load.client_saturated,
         "load generator CPU is saturated -- it, not the server, may be the bottleneck");
    flag(res.major_faults > 0,
         "major page faults occurred -- memory was paged from disk during the run");

    // Scheduler interference, measured rather than inferred from a platform
    // counter. The simulation asks to be woken at a fixed rate; how late it
    // actually starts is a direct, portable reading of how much the scheduler
    // is delaying a runnable thread. Anything past a quarter of the tick period
    // at p99 means the machine, not the server, owns the tail.
    if (have_server && tick_hz > 0 && srv.tick_jitter.count > 0) {
        const std::uint64_t period_ns = std::uint64_t(1'000'000'000 / tick_hz);
        const double jitter_frac = double(srv.tick_jitter.p99) / double(period_ns);
        if (jitter_frac > 0.25) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                "scheduler interference: simulation ticks start %s late at p99 (%.0f%% of the "
                "%s period).\n         The machine is contended -- p50 is still meaningful, the "
                "tails are not.",
                dur(srv.tick_jitter.p99).c_str(), jitter_frac * 100.0, dur(period_ns).c_str());
            flag(true, buf);
        }
    }
    flag(!env.threads_pinned,
         "threads are not pinned to cores; scheduler migration widens the tail on this platform");
    flag(load.ping_scheduled.count < 10'000,
         "fewer than 10k latency samples -- p99.99 is not resolvable at this sample count");
    flag(load.achieved_msgs_per_s < load.offered_msgs_per_s * 0.99,
         "the generator did not deliver the offered load; throughput is client-limited");
#ifndef NDEBUG
    flag(true, "assertions are enabled -- this is not an optimised build");
#endif
    if (clean) std::printf("  \033[32mno validity warnings\033[0m\n");
}

void print_sweep(const std::vector<SweepPoint>& pts) {
    rule("LOAD SWEEP  (latency as a function of offered throughput)");
    std::printf("  %12s %12s %10s %10s %10s %10s %7s %7s %6s %s\n",
                "offered/s", "achieved/s", "p50", "p99", "p99.9", "max", "cores", "drops",
                "deliv", "");
    for (const auto& p : pts) {
        std::printf("  %12s %12s %10s %10s %10s %10s %7.2f %7" PRIu64 " %5.1f%% %s\n",
                    count(p.offered_msgs_per_s).c_str(), count(p.achieved_msgs_per_s).c_str(),
                    dur(p.p50).c_str(), dur(p.p99).c_str(), dur(p.p999).c_str(), dur(p.max).c_str(),
                    p.cores_used, p.drops, p.fidelity_pct,
                    p.client_limited ? "<- generator CPU-bound; measures the client" : "");
    }
    // The knee is where queueing begins: the last point whose p99 is still
    // within 2x of the lightly loaded p99. Past it, adding load buys latency,
    // not throughput.
    if (pts.size() >= 2) {
        // Usable capacity: the highest offered rate that still delivered its
        // schedule and kept p99 within 2x of the lightly loaded value. Points
        // where the generator was CPU-bound are excluded -- they describe the
        // measuring instrument, not the server.
        const std::uint64_t base = pts.front().p99;
        bool found = false;
        std::size_t knee = 0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (pts[i].client_limited) continue;
            if (pts[i].fidelity_pct < 99.0) continue;
            if (pts[i].p99 > base * 2) continue;
            knee = i;
            found = true;
        }
        if (found)
            std::printf("\n  usable capacity       %s msg/s delivered, p99 %s\n"
                        "                        (highest rate holding schedule with p99 within 2x of baseline)\n",
                        count(pts[knee].achieved_msgs_per_s).c_str(), dur(pts[knee].p99).c_str());
        else
            std::printf("\n  usable capacity       not resolved: no sweep point held both its schedule\n"
                        "                        and a p99 within 2x of baseline. Widen the sweep downward.\n");
    }
}

bool write_sweep_csv(const std::string& path, const std::vector<SweepPoint>& pts) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "offered_msgs_per_s,achieved_msgs_per_s,p50_ns,p99_ns,p999_ns,max_ns,"
                    "cores_used,drops,fidelity_pct,client_limited\n");
    for (const auto& p : pts)
        std::fprintf(f, "%.1f,%.1f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.4f,%" PRIu64 ",%.2f,%d\n",
                     p.offered_msgs_per_s, p.achieved_msgs_per_s, p.p50, p.p99, p.p999, p.max,
                     p.cores_used, p.drops, p.fidelity_pct, p.client_limited ? 1 : 0);
    std::fclose(f);
    return true;
}

bool write_histogram_csv(const std::string& path,
                         const std::vector<std::pair<std::string, const metrics::Histogram*>>& series) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "series,bucket_lo_ns,bucket_hi_ns,count,cumulative,cumulative_pct\n");
    for (const auto& [name, h] : series) {
        if (!h || h->count() == 0) continue;
        const double total = double(h->count());
        h->for_each_bucket([&](std::uint64_t lo, std::uint64_t hi,
                               std::uint64_t n, std::uint64_t cum) {
            std::fprintf(f, "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f\n",
                         name.c_str(), lo, hi, n, cum, 100.0 * double(cum) / total);
        });
    }
    std::fclose(f);
    return true;
}

namespace {
void json_latency(std::FILE* f, const char* name, const metrics::LatencySummary& s, bool last) {
    std::fprintf(f,
        "    \"%s\": {\"count\":%" PRIu64 ",\"min_ns\":%" PRIu64 ",\"p50_ns\":%" PRIu64
        ",\"p90_ns\":%" PRIu64 ",\"p95_ns\":%" PRIu64 ",\"p99_ns\":%" PRIu64
        ",\"p999_ns\":%" PRIu64 ",\"p9999_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64
        ",\"mean_ns\":%.1f,\"stddev_ns\":%.1f}%s\n",
        name, s.count, s.min, s.p50, s.p90, s.p95, s.p99, s.p999, s.p9999, s.max,
        s.mean, s.stddev, last ? "" : ",");
}
} // namespace

bool write_json(const std::string& path, const RunEnvironment& env, const LoadConfig& cfg,
                const LoadReport& load, const server::ServerReport& srv,
                const metrics::ResourceDelta& res) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    const double msgs = double(load.pings_sent + load.moves_sent + load.pongs_received + load.deltas_received);

    std::fprintf(f, "{\n");
    // The backend is a runtime choice (--io=poll|uring|auto), so it cannot live in
    // env.os, which is fixed at compile time: that stamped every Linux artifact
    // "epoll" whatever actually ran, leaving two runs of different backends
    // indistinguishable except by a filename someone typed by hand.
    std::fprintf(f, "  \"environment\": {\"arch\":\"%s\",\"os\":\"%s\",\"io_backend\":\"%s\","
                    "\"compiler\":\"%s\",\"build\":\"%s\",\"logical_cores\":%u,"
                    "\"performance_cores\":%u,\"clock_overhead_ns\":%lld,"
                    "\"clock_granularity_ns\":%lld,\"threads_pinned\":%s,"
                    "\"reuseport\":%s,\"placement\":\"%s\"},\n",
                 env.host_arch.c_str(), env.os.c_str(),
                 srv.io_backend.empty() ? "unknown" : srv.io_backend.c_str(),
                 env.compiler.c_str(), env.build_type.c_str(),
                 env.logical_cores, env.performance_cores,
                 static_cast<long long>(env.clock_overhead_ns),
                 static_cast<long long>(env.clock_granularity_ns),
                 env.threads_pinned ? "true" : "false",
                 srv.used_reuseport ? "true" : "false",
                 srv.placement.c_str());
    std::fprintf(f, "  \"config\": {\"clients\":%u,\"client_threads\":%u,\"ping_rate_hz\":%.1f,"
                    "\"move_rate_hz\":%.1f,\"duration_s\":%.1f,\"warmup_s\":%.1f,"
                    "\"poisson\":%s,\"seed\":%llu},\n",
                 cfg.clients, cfg.threads, cfg.ping_rate_hz, cfg.move_rate_hz,
                 cfg.duration_s, cfg.warmup_s, cfg.poisson ? "true" : "false",
                 static_cast<unsigned long long>(cfg.seed));
    std::fprintf(f, "  \"latency\": {\n");
    json_latency(f, "ping_scheduled", load.ping_scheduled, false);
    json_latency(f, "ping_wire",      load.ping_wire, false);
    json_latency(f, "e2e_scheduled",  load.e2e_scheduled, false);
    json_latency(f, "send_delay",     load.send_delay, false);
    json_latency(f, "server_ping_service", srv.ping_service, false);
    json_latency(f, "server_loop_work", srv.loop_work, false);
    json_latency(f, "shard_tick_jitter", srv.tick_jitter, false);
    json_latency(f, "shard_tick_duration", srv.tick_duration, false);
    json_latency(f, "io_to_shard_transit", srv.queue_transit, true);
    std::fprintf(f, "  },\n");
    std::fprintf(f, "  \"throughput\": {\"offered_msgs_per_s\":%.1f,\"achieved_msgs_per_s\":%.1f,"
                    "\"reply_msgs_per_s\":%.1f,\"total_msgs\":%.0f},\n",
                 load.offered_msgs_per_s, load.achieved_msgs_per_s, load.reply_msgs_per_s, msgs);
    std::fprintf(f, "  \"allocations\": {\"probe_enabled\":%s,\"steady_state_allocs\":%" PRIu64
                    ",\"steady_state_frees\":%" PRIu64 ",\"steady_state_bytes\":%" PRIu64
                    ",\"process_total_allocs\":%" PRIu64 "},\n",
                 alloc::probe_enabled() ? "true" : "false",
                 load.client_alloc_steady.allocs, load.client_alloc_steady.frees,
                 load.client_alloc_steady.bytes_allocated, srv.alloc_total.allocs);
    std::fprintf(f, "  \"memory\": {\"rss_bytes\":%" PRIu64 ",\"peak_rss_bytes\":%" PRIu64
                    ",\"reserved_bytes\":%" PRIu64 ",\"bytes_per_connection\":%" PRIu64 "},\n",
                 res.rss_bytes, res.peak_rss_bytes, srv.reserved_bytes, srv.bytes_per_conn);
    std::fprintf(f, "  \"cpu\": {\"wall_ns\":%lld,\"cpu_ns\":%lld,\"cores_used\":%.4f,"
                    "\"cpu_us_per_message\":%.4f,\"vol_ctx_switches\":%" PRIu64
                    ",\"invol_ctx_switches\":%" PRIu64 ",\"minor_faults\":%" PRIu64
                    ",\"major_faults\":%" PRIu64 "},\n",
                 static_cast<long long>(res.wall_ns), static_cast<long long>(res.cpu_ns), res.cores_used(),
                 res.cpu_us_per_message(std::uint64_t(msgs)),
                 res.vol_ctx_switches, res.invol_ctx_switches, res.minor_faults, res.major_faults);
    std::fprintf(f, "  \"backpressure\": {\"cmd_ring_full\":%" PRIu64 ",\"deltas_dropped\":%" PRIu64
                    ",\"short_writes\":%" PRIu64 ",\"evicted_slow\":%" PRIu64
                    ",\"parse_errors\":%" PRIu64 ",\"ticks_late\":%" PRIu64
                    ",\"ticks_skipped\":%" PRIu64 "},\n",
                 std::uint64_t(srv.io.cmd_ring_full), std::uint64_t(srv.shard.deltas_dropped), std::uint64_t(srv.io.short_writes),
                 std::uint64_t(srv.io.evicted_slow), std::uint64_t(srv.io.parse_errors), std::uint64_t(srv.shard.ticks_late), std::uint64_t(srv.shard.ticks_skipped));
    std::fprintf(f, "  \"syscalls\": {\"read_calls\":%" PRIu64 ",\"write_calls\":%" PRIu64
                    ",\"poll_calls\":%" PRIu64 ",\"frames_in\":%" PRIu64 ",\"frames_out\":%" PRIu64 "}\n",
                 std::uint64_t(srv.io.read_calls), std::uint64_t(srv.io.write_calls), std::uint64_t(srv.io.poll_calls),
                 std::uint64_t(srv.io.frames_in), std::uint64_t(srv.io.frames_out));
    std::fprintf(f, "}\n");
    std::fclose(f);
    return true;
}

} // namespace ge::bench
