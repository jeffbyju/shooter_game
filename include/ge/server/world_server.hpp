#pragma once
// The world server process: an acceptor, N I/O threads, and M simulation
// shards, connected only by bounded lock-free rings.
//
//   acceptor ──MpmcQueue<fd>──▶ I/O thread i ──SpscRing<Command>──▶ shard j
//                                    ▲                                  │
//                                    └────────SpscByteRing──────────────┘
//
// Thread count is a configuration input, never derived from load at runtime:
// a thread pool that grows under pressure adds scheduling latency exactly when
// the system can least afford it.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ge/alloc/arena.hpp"
#include "ge/alloc/counters.hpp"
#include "ge/core/error.hpp"
#include "ge/core/topology.hpp"
#include "ge/container/mpmc_queue.hpp"
#include "ge/metrics/counters.hpp"
#include "ge/metrics/histogram.hpp"
#include "ge/metrics/process.hpp"
#include "ge/net/connection.hpp"
#include "ge/net/reactor.hpp"
#include "ge/net/socket.hpp"
#include "ge/proto/codec.hpp"
#include "ge/world/shard.hpp"

namespace ge::server {

struct ServerConfig {
    std::string   host            = "127.0.0.1";
    std::uint16_t port            = 9000;
    std::uint16_t io_threads      = 2;
    std::uint16_t shards          = 2;
    int           tick_hz         = 60;
    std::uint32_t conns_per_io    = 512;
    // listen() backlog. 1024 was not enough: three load generators opening
    // ~2000 connections at once overflowed the accept queue and the kernel
    // reset the overflow, which clients saw as a connection that died during
    // the handshake.
    int           backlog         = 1024;
    std::uint32_t read_buf_bytes  = 4096;
    std::uint32_t write_buf_bytes = 16384;    // power of two required
    int           poll_timeout_ms = 50;
    // Adaptive spin: after an event, keep polling with a zero timeout for this
    // long before falling back to a blocking wait.
    //
    // Waking a core that has entered a deep idle state costs on the order of
    // 100us on this class of hardware -- an order of magnitude more than the
    // entire rest of the request path. At low offered load that wakeup *is* the
    // latency. Spinning keeps the core hot; spinning forever burns it. Spinning
    // only while the connection is plausibly active buys most of the latency
    // back for a fraction of the CPU, and the benchmark reports both sides of
    // that trade rather than picking one and calling it fast.
    std::int64_t  io_spin_ns      = 0;
    bool          busy_poll_shards= false;
    bool          pin_threads     = true;
    // Use one SO_REUSEPORT listener per I/O thread instead of a single acceptor
    // thread feeding a handoff queue. Removes a thread, a queue and a wakeup
    // from the connection path. Ignored where the kernel does not actually
    // balance across shared listeners (see net::reuseport_balances_connections).
    bool          reuseport       = true;
    // Which kernel interface the I/O threads wait on. Defaults to Poll
    // (epoll/kqueue); Uring is implemented but unvalidated. See
    //
    net::Backend  io_backend      = net::Backend::Poll;
    // Which simulation the shards run. Snake is the default so every existing
    // caller, test and benchmark keeps its behaviour without being touched.
    world::GameMode mode          = world::GameMode::Snake;
    float         world_size      = 1024.0f;
    float         cell_size       = 48.0f;
    float         aoi_radius      = 64.0f;
    std::size_t   max_entities_per_shard = 8192;
    std::size_t   out_ring_bytes  = 4u << 20;
    std::int64_t  idle_timeout_ns = 30'000'000'000;
};

struct ServerReport {
    metrics::IoCounters      io;
    metrics::ShardCounters   shard;
    metrics::LatencySummary  ping_service;   // frame parsed -> response handed to the kernel
    metrics::LatencySummary  loop_work;      // excludes time blocked in poll()
    metrics::LatencySummary  tick_jitter;
    metrics::LatencySummary  tick_duration;
    metrics::LatencySummary  queue_transit;  // I/O thread -> shard, one direction
    std::uint64_t            live_connections = 0;
    std::uint64_t            live_entities    = 0;
    std::uint64_t            reserved_bytes   = 0;   // everything the server mapped up front
    // How connections actually arrived, so the report never implies a design
    // that was silently unavailable.
    bool                     used_reuseport   = false;
    bool                     threads_pinned   = false;
    std::string              placement;               // human-readable thread->cpu plan
    std::string              io_backend;              // what the reactors actually used
    std::uint64_t            bytes_per_conn   = 0;
    alloc::AllocStats        alloc_total;
};

class WorldServer {
public:
    WorldServer();
    ~WorldServer();
    WorldServer(const WorldServer&)            = delete;
    WorldServer& operator=(const WorldServer&) = delete;

    core::Status start(const ServerConfig& cfg);
    void stop();
    void join();

    std::uint16_t port() const noexcept { return bound_port_; }

    // Snapshot of everything the counters know. Safe to call while running.
    ServerReport report() const;

    // Resets latency histograms and counters. Used to discard warmup so the
    // reported distribution is steady state only.
    void reset_measurements();

    // Writes the server-side latency distributions as CSV, one row per occupied
    // histogram bucket. Aggregates across threads and shards first.
    bool write_histogram_csv(const std::string& path) const;

private:
    struct IoThread;

    void acceptor_loop();
    void io_loop(IoThread& t);
    void handle_readable(IoThread& t, net::Connection& c);
    void handle_frame(IoThread& t, net::Connection& c, const proto::FrameView& f);
    bool drain_shard_output(IoThread& t);   // true if output remains queued
    void close_connection(IoThread& t, net::Connection& c);

    ServerConfig                 cfg_{};
    net::Fd                      listen_fd_;
    std::uint16_t                bound_port_ = 0;
    std::atomic<bool>            running_{false};
    // Accepted connections dropped because an I/O thread's handoff ring was
    // full. Silent before it was counted.
    std::atomic<std::uint64_t>   handoff_full_{0};
    std::atomic<std::uint32_t>   next_session_{1};

    std::vector<std::unique_ptr<IoThread>>            io_;
    std::vector<std::unique_ptr<world::WorldShard>>   shards_;
    std::vector<std::thread>                          threads_;
    std::thread                                       acceptor_;
    std::vector<int>                                  cpu_plan_;
    bool                                              use_reuseport_ = false;
};

} // namespace ge::server
