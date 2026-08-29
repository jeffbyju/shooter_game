// ge_loadgen -- benchmark driver.
//
// Default mode runs the server in-process so a single command produces a full,
// self-consistent report. Point it at --host/--port to measure a server running
// elsewhere; in that case the server-side counters are unavailable and the
// report says so instead of printing zeros.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ge/bench/loadgen.hpp"
#include "ge/bench/report.hpp"
#include "ge/core/clock.hpp"
#include "ge/metrics/perf_counters.hpp"
#include "ge/metrics/process.hpp"
#include "ge/server/world_server.hpp"

namespace {

struct Args {
    ge::bench::LoadConfig   load;
    ge::server::ServerConfig server;
    bool        inproc     = true;
    std::string json_path;
    std::string sweep_csv;
    std::string hist_prefix;
    std::vector<double> sweep_rates;
};

[[noreturn]] void usage(const char* argv0) {
    std::printf(
"usage: %s [options]\n"
"\n"
"  Load\n"
"    --clients N         concurrent client connections      (default 100)\n"
"    --client-threads N  load generator threads             (default 2)\n"
"    --ping-rate HZ      pings per client per second        (default 50)\n"
"    --move-rate HZ      inputs per client per second       (default 30)\n"
"    --duration S        measurement window                 (default 10)\n"
"    --warmup S          discarded before measuring         (default 2)\n"
"    --uniform           evenly spaced sends instead of Poisson arrivals\n"
"    --seed N            PRNG seed; a run is reproducible from it\n"
"\n"
"  Server (in-process unless --host is given)\n"
"    --host H            measure an external server instead\n"
"    --port P            port                               (default ephemeral)\n"
"    --io-threads N      server I/O threads                 (default 2)\n"
"    --shards N          simulation threads                 (default 2)\n"
"    --tick-hz N         simulation rate                    (default 60)\n"
"    --busy-poll         shards spin instead of sleeping between ticks\n"
"    --poll-timeout MS   server event loop timeout          (default 50)\n"
"    --io-backend B      poll (default) | uring | auto. uring is implemented\n"
"                        but UNVALIDATED; see the load notes\n"
"    --spin-us N         I/O threads spin for N us after an event before\n"
"                        blocking; trades CPU for idle-core wakeup latency\n"
"\n"
"  Output\n"
"    --json PATH         write the full report as JSON\n"
"    --sweep A,B,C       sweep offered ping rates and print a latency curve\n"
"    --sweep-csv PATH    write the sweep as CSV\n"
"    --hist-csv PREFIX   dump full latency distributions as PREFIX-client.csv\n"
"                        and PREFIX-server.csv, one row per histogram bucket,\n"
"                        so the CDF can be plotted rather than inferred from\n"
"                        seven percentiles\n", argv0);
    std::exit(2);
}

bool eat(int& i, int argc, char** argv, const char* name, const char** out) {
    if (std::strcmp(argv[i], name) != 0) return false;
    if (i + 1 >= argc) usage(argv[0]);
    *out = argv[++i];
    return true;
}

Args parse(int argc, char** argv) {
    Args a;
    a.load.clients = 100;
    a.load.threads = 2;
    a.server.port  = 0;          // ephemeral by default: never collides with a live service
    const char* v = nullptr;

    for (int i = 1; i < argc; ++i) {
        if      (eat(i, argc, argv, "--clients", &v))        a.load.clients = std::uint32_t(std::atoi(v));
        else if (eat(i, argc, argv, "--client-threads", &v)) a.load.threads = std::uint32_t(std::atoi(v));
        else if (eat(i, argc, argv, "--ping-rate", &v))      a.load.ping_rate_hz = std::atof(v);
        else if (eat(i, argc, argv, "--move-rate", &v))      a.load.move_rate_hz = std::atof(v);
        else if (eat(i, argc, argv, "--duration", &v))       a.load.duration_s = std::atof(v);
        else if (eat(i, argc, argv, "--warmup", &v))         a.load.warmup_s = std::atof(v);
        else if (eat(i, argc, argv, "--seed", &v))           a.load.seed = std::strtoull(v, nullptr, 0);
        else if (eat(i, argc, argv, "--host", &v))         { a.load.host = v; a.inproc = false; }
        else if (eat(i, argc, argv, "--port", &v))         { a.load.port = std::uint16_t(std::atoi(v));
                                                             a.server.port = a.load.port; }
        else if (eat(i, argc, argv, "--io-threads", &v))     a.server.io_threads = std::uint16_t(std::atoi(v));
        else if (eat(i, argc, argv, "--shards", &v))         a.server.shards = std::uint16_t(std::atoi(v));
        else if (eat(i, argc, argv, "--tick-hz", &v))        a.server.tick_hz = std::atoi(v);
        else if (eat(i, argc, argv, "--poll-timeout", &v))   a.server.poll_timeout_ms = std::atoi(v);
        else if (eat(i, argc, argv, "--spin-us", &v))        a.server.io_spin_ns = std::atoll(v) * 1000;
        else if (eat(i, argc, argv, "--io-backend", &v)) {
            if      (!std::strcmp(v, "poll"))  a.server.io_backend = ge::net::Backend::Poll;
            else if (!std::strcmp(v, "uring")) a.server.io_backend = ge::net::Backend::Uring;
            else if (!std::strcmp(v, "auto"))  a.server.io_backend = ge::net::Backend::Auto;
            else usage(argv[0]);
        }
        else if (eat(i, argc, argv, "--json", &v))           a.json_path = v;
        else if (eat(i, argc, argv, "--sweep-csv", &v))      a.sweep_csv = v;
        else if (eat(i, argc, argv, "--hist-csv", &v))       a.hist_prefix = v;
        else if (eat(i, argc, argv, "--sweep", &v)) {
            for (const char* p = v; *p;) {
                a.sweep_rates.push_back(std::atof(p));
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
            }
        }
        else if (std::strcmp(argv[i], "--uniform") == 0)   a.load.poisson = false;
        else if (std::strcmp(argv[i], "--busy-poll") == 0) a.server.busy_poll_shards = true;
        else usage(argv[0]);
    }
    auto bad = [&](const char* what) {
        std::fprintf(stderr, "ge_loadgen: %s\n", what);
        std::exit(2);
    };
    if (a.server.tick_hz <= 0)        bad("--tick-hz must be > 0");
    if (a.server.io_threads == 0)     bad("--io-threads must be >= 1");
    if (a.server.shards == 0)         bad("--shards must be >= 1");
    if (a.load.clients == 0)          bad("--clients must be >= 1");
    if (a.load.threads == 0)          bad("--client-threads must be >= 1");
    if (a.load.duration_s <= 0)       bad("--duration must be > 0");
    if (a.load.warmup_s < 0)          bad("--warmup must be >= 0");
    if (a.load.ping_rate_hz < 0 || a.load.move_rate_hz < 0)
        bad("rates must be >= 0");
    if (a.load.ping_rate_hz == 0 && a.load.move_rate_hz == 0)
        bad("--ping-rate and --move-rate cannot both be 0: no load would be offered");

    // The server must be able to hold every client on a single I/O thread in the
    // worst case; sizing it here beats discovering it as a refused connection.
    a.server.conns_per_io = std::max<std::uint32_t>(64, a.load.clients + 16);
    return a;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse(argc, argv);

    ge::bench::RunEnvironment env = ge::bench::probe_environment();
    env.threads_pinned = false;
#if defined(GE_OS_LINUX)
    env.threads_pinned = args.server.pin_threads;
#endif
    ge::bench::print_environment(env);

    ge::server::WorldServer server;
    if (args.inproc) {
        if (auto started = server.start(args.server); !started) {
            std::fprintf(stderr, "server start failed: %s\n", started.error().c_str());
            return 1;
        }
        args.load.port = server.port();
        std::printf("\n  in-process server     %u I/O threads, %u shards, %d Hz tick, port %u\n",
                    args.server.io_threads, args.server.shards, args.server.tick_hz, server.port());
    } else {
        std::printf("\n  external server       %s:%u (server-side counters unavailable)\n",
                    args.load.host.c_str(), args.load.port);
    }

    // ---------------------------------------------------------------- sweep
    if (!args.sweep_rates.empty()) {
        std::vector<ge::bench::SweepPoint> points;
        for (double rate : args.sweep_rates) {
            ge::bench::LoadConfig c = args.load;
            c.ping_rate_hz = rate;
            server.reset_measurements();

            const auto before = ge::metrics::sample_process();
            const auto result = ge::bench::run_load(c);
            const auto after = ge::metrics::sample_process();
            if (!result) {
                std::fprintf(stderr, "sweep point %.0f Hz failed: %s\n",
                             rate, result.error().c_str());
                continue;
            }
            const ge::bench::LoadReport& r = *result;
            const auto srv = server.report();
            const auto res = ge::metrics::diff(before, after);
            ge::bench::SweepPoint pt;
            pt.offered_msgs_per_s  = r.offered_msgs_per_s;
            pt.achieved_msgs_per_s = r.achieved_msgs_per_s;
            pt.p50   = r.ping_scheduled.p50;
            pt.p99   = r.ping_scheduled.p99;
            pt.p999  = r.ping_scheduled.p999;
            pt.max   = r.ping_scheduled.max;
            pt.cores_used = res.cores_used();
            pt.drops = srv.shard.deltas_dropped + srv.io.cmd_ring_full;
            pt.client_limited = r.client_saturated;
            pt.fidelity_pct = r.offered_msgs_per_s > 0
                            ? 100.0 * r.achieved_msgs_per_s / r.offered_msgs_per_s : 0.0;
            points.push_back(pt);
        }
        ge::bench::print_sweep(points);
        if (!args.sweep_csv.empty() && ge::bench::write_sweep_csv(args.sweep_csv, points))
            std::printf("\n  sweep written to %s\n", args.sweep_csv.c_str());
        server.stop();
        server.join();
        return 0;
    }

    // ---------------------------------------------------------- single run
    std::printf("  load                  %u clients over %u threads, %.0f ping/s + %.0f move/s each\n",
                args.load.clients, args.load.threads, args.load.ping_rate_hz, args.load.move_rate_hz);
    std::printf("  schedule              %s arrivals, seed 0x%llx\n",
                args.load.poisson ? "Poisson (exponential gaps)" : "uniform",
                static_cast<unsigned long long>(args.load.seed));
    std::printf("  window                %.1fs warmup (discarded) + %.1fs measured\n",
                args.load.warmup_s, args.load.duration_s);
    std::fflush(stdout);

    if (!args.hist_prefix.empty()) args.load.histogram_csv_path = args.hist_prefix + "-client.csv";

    ge::metrics::PerfCounters perf;
    const auto before  = ge::metrics::sample_process();
    perf.start();
    const auto measured = ge::bench::run_load(args.load);
    perf.stop();
    const ge::metrics::PerfSample hw = perf.read();
    const auto after   = ge::metrics::sample_process();

    if (!measured) {
        std::fprintf(stderr, "\nload generator failed: %s\n", measured.error().c_str());
        server.stop(); server.join();
        return 1;
    }
    const ge::bench::LoadReport& load = *measured;

    const ge::server::ServerReport srv = args.inproc ? server.report() : ge::server::ServerReport{};
    const ge::metrics::ResourceDelta res = ge::metrics::diff(before, after);

    ge::bench::print_throughput(load, srv, args.inproc);

    std::printf("\n\033[1mLATENCY DISTRIBUTIONS\033[0m\n");
    std::printf("--------------------------------------------------------------------------\n");
    ge::bench::print_latency("ping RTT (scheduled)", load.ping_scheduled,
        "client scheduled-send -> reply. Includes client-side lateness, so this is what a user feels.");
    ge::bench::print_latency("ping RTT (wire)", load.ping_wire,
        "client actual-send -> reply. Excludes generator lateness; isolates server + network.");
    ge::bench::print_latency("input -> state", load.e2e_scheduled,
        "MoveCommand -> the first StateDelta reflecting it. Includes up to one tick of quantisation.");
    if (args.inproc) {
        ge::bench::print_latency("  upstream (in)", load.upstream,
            "client send -> server receive. Same process, same clock, so this split is exact.");
        ge::bench::print_latency("  downstream (out)", load.downstream,
            "server receive -> client receive: server work plus the client's own wakeup.");
    }
    ge::bench::print_latency("generator send delay", load.send_delay,
        "how late the load generator issued each request. Large values invalidate the offered load.");
    if (args.inproc) {
        ge::bench::print_latency("server ping service", srv.ping_service,
            "frame parsed -> reply handed to the kernel, on the I/O thread. Pure server-side cost.");
        ge::bench::print_latency("io loop work", srv.loop_work,
            "work per event-loop pass: dispatch plus draining simulation output. Excludes time blocked in poll().");
        ge::bench::print_latency("shard tick jitter", srv.tick_jitter,
            "how late each tick started against its fixed deadline. Drift-free schedule, hybrid wait.");
        ge::bench::print_latency("shard tick duration", srv.tick_duration,
            "work inside one tick: apply inputs, integrate, rebuild grid, build deltas.");
        ge::bench::print_latency("io -> shard transit", srv.queue_transit,
            "time an input spent in the SPSC ring between the I/O thread and the simulation.");
    }

    ge::bench::print_client_observations(load);
    ge::bench::print_allocations(load, srv);
    ge::bench::print_memory(srv, after, load.clients_connected);
    ge::bench::print_cpu(res, load, srv, env);
    ge::bench::print_hardware_counters(
        hw, load.pings_sent + load.moves_sent + load.pongs_received + load.deltas_received);
    if (args.inproc) {
        ge::bench::print_syscalls(srv);
        ge::bench::print_backpressure(srv, load);
    }
    ge::bench::print_validity(res, load, env, srv, args.server.tick_hz, args.inproc);

    if (!args.hist_prefix.empty()) {
        std::printf("\n  distributions written to %s-client.csv", args.hist_prefix.c_str());
        if (args.inproc && server.write_histogram_csv(args.hist_prefix + "-server.csv"))
            std::printf(" and %s-server.csv", args.hist_prefix.c_str());
        std::printf("\n");
    }

    if (!args.json_path.empty()) {
        if (ge::bench::write_json(args.json_path, env, args.load, load, srv, res))
            std::printf("\n  report written to %s\n", args.json_path.c_str());
        else
            std::fprintf(stderr, "\n  could not write %s\n", args.json_path.c_str());
    }

    server.stop();
    server.join();
    std::printf("\n");
    return 0;
}
