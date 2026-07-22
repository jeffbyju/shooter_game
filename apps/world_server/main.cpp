// ge_world_server -- standalone backend instance.
//
// Prints a live counter line every second so the process is observable without
// attaching a profiler, and a full report on shutdown.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "ge/bench/report.hpp"
#include "ge/metrics/process.hpp"
#include "ge/server/world_server.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_release); }

[[noreturn]] void usage(const char* a0) {
    std::printf(
"usage: %s [options]\n"
"    --host H          bind address           (default 127.0.0.1)\n"
"    --port P          bind port              (default 9000, 0 = ephemeral)\n"
"    --io-threads N    I/O threads            (default 2)\n"
"    --shards N        simulation threads     (default 2)\n"
"    --tick-hz N       simulation rate        (default 60)\n"
"    --max-conns N     connections per I/O thread (default 512)\n"
"    --world W         world is W x W units        (default 1024)\n"
"    --aoi R           how far a player can see    (default 64)\n"
"\n"
"  To watch a shared world with ge_viewer, players must be on one shard and\n"
"  able to see each other:\n"
"    ge_world_server --port 9000 --shards 1 --world 256 --aoi 512\n"
"    --io-backend B    poll (default) | uring | auto; uring is UNVALIDATED\n"
"    --busy-poll       shards spin between ticks instead of sleeping\n"
"    --spin-us N       I/O threads spin N us after an event before blocking\n"
"    --quiet           no per-second counter line\n", a0);
    std::exit(2);
}

} // namespace

int main(int argc, char** argv) {
    ge::server::ServerConfig cfg;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { if (i + 1 >= argc) usage(argv[0]); return argv[++i]; };
        if      (!std::strcmp(argv[i], "--host"))       cfg.host = next();
        else if (!std::strcmp(argv[i], "--port"))       cfg.port = std::uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--io-threads")) cfg.io_threads = std::uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--shards"))     cfg.shards = std::uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--tick-hz"))    cfg.tick_hz = std::atoi(next());
        else if (!std::strcmp(argv[i], "--max-conns"))  cfg.conns_per_io = std::uint32_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--world"))      cfg.world_size = float(std::atof(next()));
        else if (!std::strcmp(argv[i], "--aoi"))        cfg.aoi_radius = float(std::atof(next()));
        else if (!std::strcmp(argv[i], "--io-backend")) {
            const char* v = next();
            if      (!std::strcmp(v, "poll"))  cfg.io_backend = ge::net::Backend::Poll;
            else if (!std::strcmp(v, "uring")) cfg.io_backend = ge::net::Backend::Uring;
            else if (!std::strcmp(v, "auto"))  cfg.io_backend = ge::net::Backend::Auto;
            else usage(argv[0]);
        }
        else if (!std::strcmp(argv[i], "--busy-poll"))  cfg.busy_poll_shards = true;
        else if (!std::strcmp(argv[i], "--spin-us"))    cfg.io_spin_ns = std::atoll(next()) * 1000;
        else if (!std::strcmp(argv[i], "--quiet"))      quiet = true;
        else usage(argv[0]);
    }

    auto bad = [&](const char* what) {
        std::fprintf(stderr, "ge_world_server: %s\n", what);
        std::exit(2);
    };
    if (cfg.tick_hz <= 0)      bad("--tick-hz must be > 0");
    if (cfg.io_threads == 0)   bad("--io-threads must be >= 1");
    if (cfg.shards == 0)       bad("--shards must be >= 1");
    if (cfg.conns_per_io == 0) bad("--max-conns must be >= 1");
    if (!(cfg.world_size > 0))  bad("--world must be > 0");
    if (!(cfg.aoi_radius > 0))  bad("--aoi must be > 0");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);   // peer disconnects are handled as EPIPE, not as death

    ge::server::WorldServer server;
    if (auto started = server.start(cfg); !started) {
        std::fprintf(stderr, "start failed: %s\n", started.error().c_str());
        return 1;
    }
    std::printf("ge_world_server listening on %s:%u -- %u I/O threads, %u shards, %d Hz\n"
                "  world %.0fx%.0f, area of interest %.0f\n",
                cfg.host.c_str(), server.port(), cfg.io_threads, cfg.shards, cfg.tick_hz,
                double(cfg.world_size), double(cfg.world_size), double(cfg.aoi_radius));
    // The two ways a connected client ends up seeing an empty world. Both are
    // reasonable defaults for a benchmark and surprising for a spectator, so
    // they are called out at startup rather than left to be discovered.
    if (cfg.shards > 1)
        std::printf("  note: %u shards -- players are split across them by session and\n"
                    "        cannot see each other. Use --shards 1 for a single shared world.\n",
                    cfg.shards);
    if (cfg.aoi_radius < cfg.world_size)
        std::printf("  note: area of interest (%.0f) is smaller than the world (%.0f), so two\n"
                    "        players are rarely in range. Use --aoi %.0f to make everyone visible.\n",
                    double(cfg.aoi_radius), double(cfg.world_size), double(cfg.world_size * 2));
    std::fflush(stdout);

    const auto t_start = ge::metrics::sample_process();
    auto prev = server.report();

    while (!g_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = server.report();
        if (!quiet) {
            const auto proc = ge::metrics::sample_process();
            std::printf("conns %4llu  entities %5llu  in %7llu/s  out %7llu/s  "
                        "ticks %4llu/s  drops %llu  rss %s\n",
                        static_cast<unsigned long long>(now.live_connections),
                        static_cast<unsigned long long>(now.live_entities),
                        static_cast<unsigned long long>(now.io.frames_in  - prev.io.frames_in),
                        static_cast<unsigned long long>(now.io.frames_out - prev.io.frames_out),
                        static_cast<unsigned long long>(now.shard.ticks   - prev.shard.ticks),
                        static_cast<unsigned long long>(now.shard.deltas_dropped + now.io.cmd_ring_full),
                        ge::metrics::format_bytes(proc.rss_bytes).c_str());
            std::fflush(stdout);
        }
        prev = now;
    }

    const auto t_end = ge::metrics::sample_process();
    const auto srv   = server.report();
    server.stop();
    server.join();

    std::printf("\n");
    ge::bench::print_latency("server ping service", srv.ping_service,
        "frame parsed -> reply handed to the kernel.");
    ge::bench::print_latency("shard tick jitter", srv.tick_jitter,
        "tick start vs its fixed deadline.");
    ge::bench::print_latency("shard tick duration", srv.tick_duration,
        "work performed inside one tick.");
    ge::bench::print_memory(srv, t_end, srv.live_connections);
    ge::bench::print_syscalls(srv);
    ge::bench::print_backpressure(srv, ge::bench::LoadReport{});
    const auto res = ge::metrics::diff(t_start, t_end);
    std::printf("\n  cpu %.3f cores over %.1fs, %llu involuntary context switches\n",
                res.cores_used(), double(res.wall_ns) / 1e9,
                static_cast<unsigned long long>(res.invol_ctx_switches));
    return 0;
}
