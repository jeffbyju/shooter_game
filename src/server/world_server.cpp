#include "ge/server/world_server.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ge/core/bits.hpp"
#include "ge/core/clock.hpp"
#include "ge/core/thread.hpp"
#include "ge/bench/report.hpp"
#include "ge/proto/codec.hpp"

namespace ge::server {

using core::Clock;

// Everything one I/O thread owns. Nothing in here is shared: no locks, and the
// only cross-thread traffic is through the rings and the handoff queue.
struct WorldServer::IoThread {
    std::uint16_t             index = 0;
    net::Reactor              reactor;
    container::MpmcQueue<int> handoff{256};   // accepted fds waiting for adoption
    net::Fd                   listener;       // own SO_REUSEPORT listener, if enabled
    int                       cpu = -1;       // the core it was pinned to, or -1

    // Connections live in a fixed slab addressed by index, not in an object
    // pool. The distinction matters: a shard holds a connection index across a
    // thread boundary for as long as a state delta is in flight, so the slot
    // must stay readable after the connection closes. An object pool threads
    // its free list through the freed object's own storage, which would turn
    // that cross-thread read into a use-after-free that resolves to a plausible
    // file descriptor. Here a closed slot simply reads back fd == -1.
    std::vector<net::Connection> conns;
    std::vector<std::uint32_t>   free_slots;   // LIFO of available indices
    std::vector<std::byte*>      rbufs, wbufs; // one pair per slot, mapped once
    std::vector<std::uint8_t>    interest;     // last registered mask per slot
    alloc::Arena                 buffers;

    metrics::IoCounters counters{};
    metrics::Histogram  ping_service{10'000'000ULL, 3};
    metrics::Histogram  loop_work{1'000'000'000ULL, 3};
    // Counter, not uint32: report() reads this from another thread, which is
    // the same race the metrics counters had. Relaxed atomics, free on arm64
    // and x86-64 -- see counters.hpp.
    metrics::Counter    live = 0;

    bool alive(std::uint32_t idx) const noexcept {
        return idx < conns.size() && conns[idx].fd() >= 0;
    }
};

namespace {

// Token encoding for the reactor. Connections use their pool index directly;
// the listener and the wakeup pipe get reserved values that no index can take.
constexpr std::uint64_t kListenToken = ~std::uint64_t{1};

void wake_reactor(void* ctx) noexcept {
    static_cast<net::Reactor*>(ctx)->wake();
}

} // namespace

WorldServer::WorldServer()  = default;
WorldServer::~WorldServer() { stop(); join(); }

core::Status WorldServer::start(const ServerConfig& cfg) {
    cfg_ = cfg;
    if (!core::is_pow2(cfg_.write_buf_bytes))
        return core::fail("write_buf_bytes must be a power of two");

    // SO_REUSEPORT is only taken where the kernel really distributes across
    // shared listeners. On Darwin it is "last binder wins", which would funnel
    // every connection to one thread while looking like it worked.
    use_reuseport_ = cfg_.reuseport && net::reuseport_balances_connections();

    // Getting the port is not as simple as binding one socket first.
    //
    // Linux requires that *every* socket sharing a port have SO_REUSEPORT set,
    // including the first one to bind it. An earlier version bound a plain
    // listener to fix the port and then had each thread bind the same port with
    // SO_REUSEPORT; the plain socket was already holding it without the option,
    // so every per-thread bind failed with EADDRINUSE and the server refused to
    // start. It worked on macOS only because reuseport is disabled there.
    //
    // So with reuseport there is no throwaway socket at all: the first thread's
    // own listener resolves the port (possibly from an ephemeral 0), and the
    // rest join it.
    if (!use_reuseport_) {
        auto listener = net::listen_tcp(cfg_.host.c_str(), cfg_.port, cfg_.backlog);
        if (!listener) return std::unexpected(listener.error());
        listen_fd_ = std::move(*listener);
        bound_port_ = net::local_port(listen_fd_.get());
    }

    // ---- shards -----------------------------------------------------------
    shards_.reserve(cfg_.shards);
    for (std::uint16_t s = 0; s < cfg_.shards; ++s) {
        auto shard = std::make_unique<world::WorldShard>();
        world::ShardConfig sc;
        sc.mode         = cfg_.mode;
        sc.shard_id     = s;
        sc.tick_hz      = cfg_.tick_hz;
        sc.bounds       = {0, 0, cfg_.world_size, cfg_.world_size};
        sc.cell_size    = cfg_.cell_size;
        sc.aoi_radius   = cfg_.aoi_radius;
        sc.max_entities = cfg_.max_entities_per_shard;
        sc.io_threads   = cfg_.io_threads;
        sc.conns_per_io = cfg_.conns_per_io;
        sc.out_ring_bytes = cfg_.out_ring_bytes;
        sc.busy_poll    = cfg_.busy_poll_shards;
        shard->configure(sc);
        shards_.push_back(std::move(shard));
    }

    // ---- I/O threads ------------------------------------------------------
    io_.reserve(cfg_.io_threads);
    for (std::uint16_t i = 0; i < cfg_.io_threads; ++i) {
        auto t = std::make_unique<IoThread>();
        t->index = i;
        if (auto r = t->reactor.open(cfg_.io_backend); !r) return r;

        // One mapping for every buffer this thread will ever need, bound to a
        // slot for the process lifetime. Allocating on accept would put malloc
        // on the connection path, scatter hot buffers across the heap, and --
        // as an earlier version of this file demonstrated -- leak the buffer
        // every time a connection closed.
        const std::size_t per_conn = cfg_.read_buf_bytes + cfg_.write_buf_bytes;
        t->buffers.reserve(per_conn * cfg_.conns_per_io + 4096);
        t->buffers.prefault();

        t->conns.resize(cfg_.conns_per_io);
        t->rbufs.resize(cfg_.conns_per_io);
        t->wbufs.resize(cfg_.conns_per_io);
        t->interest.assign(cfg_.conns_per_io, net::kNone);
        t->free_slots.reserve(cfg_.conns_per_io);
        if (use_reuseport_) {
            // The first thread may be resolving an ephemeral port for everyone.
            auto own = net::listen_tcp_shared(cfg_.host.c_str(),
                                              bound_port_ ? bound_port_ : cfg_.port,
                                              cfg_.backlog);
            if (!own) return std::unexpected(own.error());
            t->listener = std::move(*own);
            if (bound_port_ == 0) bound_port_ = net::local_port(t->listener.get());
        }
        for (std::uint32_t s = cfg_.conns_per_io; s-- > 0;) {
            t->rbufs[s] = static_cast<std::byte*>(t->buffers.allocate(cfg_.read_buf_bytes, 8));
            t->wbufs[s] = static_cast<std::byte*>(t->buffers.allocate(cfg_.write_buf_bytes, 8));
            GE_CHECK(t->rbufs[s] && t->wbufs[s]);
            t->conns[s].init(-1, s, t->rbufs[s], cfg_.read_buf_bytes,
                             t->wbufs[s], cfg_.write_buf_bytes);
            t->free_slots.push_back(s);
        }
        io_.push_back(std::move(t));
    }

    for (auto& s : shards_)
        for (std::uint16_t i = 0; i < cfg_.io_threads; ++i)
            s->set_waker(i, world::Waker{&wake_reactor, &io_[i]->reactor});

    // One CPU per thread, physical cores first, filling one NUMA node before
    // spilling to the next.
    const auto& topo = core::topology();
    const int total_threads = int(cfg_.shards) + int(cfg_.io_threads);
    cpu_plan_ = topo.plan(total_threads);

    running_.store(true, std::memory_order_release);

    for (auto& s : shards_)
        threads_.emplace_back([this, p = s.get()] { p->run(running_); });
    for (auto& t : io_)
        threads_.emplace_back([this, p = t.get()] { io_loop(*p); });
    // The acceptor only exists when the kernel is not doing its job for us.
    if (!use_reuseport_) acceptor_ = std::thread([this] { acceptor_loop(); });
    return core::ok();
}

void WorldServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    for (auto& t : io_) t->reactor.wake();
    listen_fd_.close();     // unblocks the acceptor's poll
}

void WorldServer::join() {
    if (acceptor_.joinable()) acceptor_.join();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
}

// ---------------------------------------------------------------- acceptor
void WorldServer::acceptor_loop() {
    core::set_thread_name("ge-accept");

    net::Reactor r;
    if (!r.open()) return;
    r.add(listen_fd_.get(), kListenToken, net::kRead);

    net::Event evs[64];
    std::uint32_t rr = 0;

    while (running_.load(std::memory_order_relaxed)) {
        const int n = r.poll(evs, 64, 100);
        if (n < 0) break;
        for (int i = 0; i < n; ++i) {
            if (evs[i].token != kListenToken) continue;
            // Drain the backlog in one wakeup: under a connection burst this is
            // the difference between accepting 1 per poll and accepting all of
            // them, and the accept queue overflowing is invisible to the client
            // except as a timeout.
            for (;;) {
                const int fd = ::accept(listen_fd_.get(), nullptr, nullptr);
                if (fd < 0) break;
                net::set_nonblocking(fd);
                net::set_nodelay(fd);
                net::set_nosigpipe(fd);

                // Round-robin. Least-loaded would need a shared counter read on
                // every accept; with homogeneous clients round-robin lands
                // within a few percent of it for free.
                IoThread& t = *io_[rr++ % io_.size()];
                if (!t.handoff.try_push(fd)) { ::close(fd); continue; }
                t.reactor.wake();
            }
        }
    }
}

// ------------------------------------------------------------- I/O threads
void WorldServer::io_loop(IoThread& t) {
    char name[32];
    std::snprintf(name, sizeof name, "ge-io-%u", unsigned(t.index));
    core::set_thread_name(name);
    core::request_perf_core(true);
    if (cfg_.pin_threads) {
        const std::size_t slot = std::size_t(cfg_.shards) + t.index;
        if (slot < cpu_plan_.size() && core::pin_to_cpu(cpu_plan_[slot]))
            t.cpu = cpu_plan_[slot];
    }
    // With a per-thread listener, this thread does its own accepting.
    if (t.listener.valid()) t.reactor.add(t.listener.get(), kListenToken, net::kRead);

    net::Event evs[256];
    std::int64_t last_event_ns = Clock::now_ns();
    bool output_pending = false;

    while (running_.load(std::memory_order_relaxed)) {
        const std::int64_t loop_start = Clock::now_ns();

        // 1. adopt newly accepted connections
        int fd;
        while (t.handoff.try_pop(fd)) {
            if (t.free_slots.empty()) { ::close(fd); continue; }  // at capacity: refuse, do not queue
            const std::uint32_t idx = t.free_slots.back();
            t.free_slots.pop_back();
            net::Connection& c = t.conns[idx];
            c.init(fd, idx, t.rbufs[idx], cfg_.read_buf_bytes,
                   t.wbufs[idx], cfg_.write_buf_bytes);
            c.touch(loop_start);
            t.reactor.add(fd, idx, net::kRead);
            t.interest[idx] = net::kRead;
            ++t.counters.accepts;
            ++t.live;
        }

        // 2. readiness. Poll with a zero timeout while inside the spin window,
        //    so the core never enters the idle state whose exit latency would
        //    dominate the measurement.
        // Never block while simulation output is still queued: that output is
        // already late by definition.
        const bool spinning = output_pending ||
                              (cfg_.io_spin_ns > 0 &&
                               (loop_start - last_event_ns) < cfg_.io_spin_ns);
        const std::int64_t poll_begin = Clock::now_ns();
        const int n = t.reactor.poll(evs, 256, spinning ? 0 : cfg_.poll_timeout_ms);
        const std::int64_t poll_end = Clock::now_ns();
        ++t.counters.poll_calls;
        if (spinning) ++t.counters.spin_polls;
        if (n == 0) ++t.counters.poll_empty;
        if (n > 0)  last_event_ns = poll_end;
        if (n < 0) break;
        // Time asleep in poll() is not work. Counting it as busy -- which the
        // obvious instrumentation does -- reports every idle server as 100%
        // utilised and makes the CPU numbers useless.
        if (!spinning) t.counters.poll_wait_ns += std::uint64_t(poll_end - poll_begin);

        for (int i = 0; i < n; ++i) {
            const net::Event& e = evs[i];
            if (e.token == net::kWakeToken) continue;
            if (e.token == kListenToken) {
                // Our own share of new connections. The kernel already chose
                // this thread by hashing the connection's 4-tuple, so there is
                // no handoff and no cross-thread wakeup.
                for (;;) {
                    const int nfd = ::accept(t.listener.get(), nullptr, nullptr);
                    if (nfd < 0) break;
                    net::set_nonblocking(nfd);
                    net::set_nodelay(nfd);
                    net::set_nosigpipe(nfd);
                    if (!t.handoff.try_push(nfd)) ::close(nfd);
                }
                continue;
            }
            const auto idx = std::uint32_t(e.token);
            if (!t.alive(idx)) continue;
            net::Connection* c = &t.conns[idx];

            if (e.writable) {
                const net::IoOutcome w = c->flush();
                if (w.result == net::IoResult::Closed || w.result == net::IoResult::Error) {
                    close_connection(t, *c);
                    continue;
                }
            }
            if (e.readable) handle_readable(t, *c);
            else if (e.hangup && !c->has_pending()) { close_connection(t, *c); continue; }

            if (c->fd() >= 0) {
                // Arm write interest only while output is actually queued.
                const std::uint8_t want = std::uint8_t(net::kRead | (c->has_pending() ? net::kWrite : 0));
                if (want != t.interest[idx]) {
                    t.reactor.modify(c->fd(), idx, want);
                    t.interest[idx] = want;
                }
            }
        }

        // 3. simulation output
        output_pending = drain_shard_output(t);

        const std::int64_t loop_end = Clock::now_ns();
        const std::uint64_t work_ns =
            std::uint64_t((poll_begin - loop_start) + (loop_end - poll_end)) +
            (spinning ? std::uint64_t(poll_end - poll_begin) : 0);
        t.loop_work.record(work_ns);
        t.counters.busy_ns += work_ns;
    }

    // Drain: close every live connection so the process exits with no leaked fds.
    for (std::uint32_t i = 0; i < cfg_.conns_per_io; ++i)
        if (t.alive(i)) close_connection(t, t.conns[i]);
}

void WorldServer::handle_readable(IoThread& t, net::Connection& c) {
    // Bounded work per connection per wakeup. An unbounded drain here would let
    // one saturating client monopolise the thread and starve every other
    // connection on it -- head-of-line blocking at the thread level.
    constexpr int kMaxReadsPerWakeup = 4;

    for (int pass = 0; pass < kMaxReadsPerWakeup; ++pass) {
        const std::uint32_t space = c.read_space();
        const net::IoOutcome r = c.fill();
        ++t.counters.read_calls;
        if (r.result == net::IoResult::Ok) t.counters.bytes_in += std::uint64_t(r.bytes);
        if (r.result == net::IoResult::Closed || r.result == net::IoResult::Error) {
            close_connection(t, c);
            return;
        }

        proto::FrameReader reader(c.readable());
        proto::FrameView   f;
        for (;;) {
            const proto::ParseError e = reader.next(f);
            if (e == proto::ParseError::Ok) {
                handle_frame(t, c, f);
                ++t.counters.frames_in;
                // handle_frame may have torn the connection down -- a Disconnect
                // message, or a send that overflowed this client's fixed output
                // budget and evicted it. The buffer has been reset underneath us,
                // so there is nothing left to parse and nothing left to consume;
                // falling through to consume(reader.consumed()) here would
                // subtract a stale byte count from a now-empty buffer.
                if (c.fd() < 0) return;
                continue;
            }
            if (e == proto::ParseError::Incomplete) break;
            // Malformed input is a protocol violation, not a recoverable state:
            // the stream is desynchronised and every later byte is suspect.
            ++t.counters.parse_errors;
            close_connection(t, c);
            return;
        }
        c.consume(reader.consumed());

        if (r.result == net::IoResult::WouldBlock) break;
        if (r.bytes == 0) break;
        // A short read on a stream socket means the receive queue is now empty.
        // Looping again would only buy an EAGAIN, and that wasted syscall is not
        // free: it was costing one extra read(2) per wakeup, which showed up in
        // the report as 0.5 frames per read() instead of 1.0.
        if (std::uint32_t(r.bytes) < space) break;
    }
}

void WorldServer::handle_frame(IoThread& t, net::Connection& c, const proto::FrameView& f) {
    const std::int64_t recv_ns = Clock::now_ns();
    c.touch(recv_ns);

    switch (f.type()) {
        case proto::MsgType::Ping: {
            // Answered entirely on the I/O thread. This is the pure transport +
            // parse + encode path with no simulation in it, and it is the number
            // that isolates server overhead from tick quantisation.
            const auto* p = f.as<proto::Ping>();
            if (!p) return;
            alignas(8) std::byte buf[sizeof(proto::FrameHeader) + sizeof(proto::Pong)];
            const proto::Pong pong{p->client_ts_ns, p->cookie, std::uint64_t(recv_ns)};
            const std::size_t n = proto::encode(buf, pong, c.session(), c.next_out_seq(),
                                                std::uint8_t(proto::FrameFlags::ServerSent));
            net::IoOutcome out{};
            if (!c.send_now(buf, std::uint32_t(n), out)) { close_connection(t, c); return; }
            ++t.counters.frames_out;
            ++t.counters.write_calls;
            if (out.result == net::IoResult::WouldBlock) ++t.counters.short_writes;
            t.ping_service.record(std::uint64_t(Clock::now_ns() - recv_ns));
            break;
        }

        case proto::MsgType::ClientHello: {
            const auto* h = f.as<proto::ClientHello>();
            if (!h || c.handshaked()) return;
            const std::uint32_t session = next_session_.fetch_add(1, std::memory_order_relaxed);
            const auto shard = std::uint16_t(session % shards_.size());
            c.set_session(session, shard);

            world::ShardCommand cmd{};
            cmd.kind       = world::ShardCommand::Kind::Join;
            cmd.io_thread  = t.index;
            cmd.conn_id    = c.id();
            cmd.session    = session;
            cmd.enqueue_ns = std::uint64_t(recv_ns);
            if (!shards_[shard]->inbox(t.index).try_push(cmd)) ++t.counters.cmd_ring_full;

            alignas(8) std::byte buf[sizeof(proto::FrameHeader) + sizeof(proto::ServerWelcome)];
            const proto::ServerWelcome w{core::mix64(session), session,
                                         cfg_.world_size, cfg_.aoi_radius,
                                         shard, std::uint16_t(shards_.size()),
                                         std::uint16_t(cfg_.tick_hz), 0, 0};
            const std::size_t n = proto::encode(buf, w, session, c.next_out_seq(),
                                                std::uint8_t(proto::FrameFlags::ServerSent));
            net::IoOutcome out{};
            if (!c.send_now(buf, std::uint32_t(n), out)) { close_connection(t, c); return; }
            ++t.counters.frames_out;
            break;
        }

        case proto::MsgType::MoveCommand: {
            const auto* m = f.as<proto::MoveCommand>();
            if (!m || !c.handshaked()) return;
            world::ShardCommand cmd{};
            cmd.kind         = world::ShardCommand::Kind::Move;
            cmd.io_thread    = t.index;
            cmd.conn_id      = c.id();
            cmd.session      = c.session();
            cmd.input_seq    = m->input_seq;
            cmd.client_ts_ns = m->client_ts_ns;
            cmd.enqueue_ns   = std::uint64_t(recv_ns);
            cmd.dx = m->dx;
            cmd.dy = m->dy;
            // A full command ring means the shard is behind. Dropping the input
            // is correct: the next one carries a newer intent, and blocking the
            // I/O thread here would convert one slow shard into a dead server.
            if (!shards_[c.shard()]->inbox(t.index).try_push(cmd)) ++t.counters.cmd_ring_full;
            break;
        }

        case proto::MsgType::ShooterInput: {
            const auto* m = f.as<proto::ShooterInput>();
            if (!m || !c.handshaked()) return;
            world::ShardCommand cmd{};
            cmd.kind         = world::ShardCommand::Kind::Input;
            cmd.io_thread    = t.index;
            cmd.conn_id      = c.id();
            cmd.session      = c.session();
            cmd.input_seq    = m->input_seq;
            cmd.client_ts_ns = m->client_ts_ns;
            cmd.enqueue_ns   = std::uint64_t(recv_ns);
            cmd.dx           = m->move_x;
            cmd.dy           = m->move_z;
            cmd.yaw          = m->yaw;
            cmd.pitch        = m->pitch;
            cmd.buttons      = m->buttons;
            // Same drop policy as MoveCommand, and for the same reason: inputs
            // are absolute, so the next one supersedes this one entirely.
            if (!shards_[c.shard()]->inbox(t.index).try_push(cmd)) ++t.counters.cmd_ring_full;
            break;
        }

        case proto::MsgType::HealthProbe: {
            const auto* p = f.as<proto::HealthProbe>();
            if (!p) return;
            alignas(8) std::byte buf[sizeof(proto::FrameHeader) + sizeof(proto::HealthReply)];
            std::uint64_t worst_fill = 0;
            for (auto& s : shards_) {
                const auto& ring = s->outbox(t.index);
                const std::uint64_t pct = ring.capacity() ? (ring.used() * 100 / ring.capacity()) : 0;
                worst_fill = std::max(worst_fill, pct);
            }
            const proto::HealthReply rep{p->probe_ts_ns, std::uint32_t(t.live.get()),
                                         std::uint16_t(worst_fill), std::uint16_t(shards_.size())};
            const std::size_t n = proto::encode(buf, rep, c.session(), c.next_out_seq(),
                                                std::uint8_t(proto::FrameFlags::ServerSent));
            net::IoOutcome out{};
            if (!c.send_now(buf, std::uint32_t(n), out)) { close_connection(t, c); return; }
            ++t.counters.frames_out;
            break;
        }

        case proto::MsgType::Disconnect:
            close_connection(t, c);
            break;

        default:
            break;   // server-to-client types arriving inbound are ignored, not fatal
    }
}

bool WorldServer::drain_shard_output(IoThread& t) {
    // Bounded, like the read path. A 60 Hz tick hands this thread one frame per
    // connection at once; draining all of them before returning to poll() makes
    // every client on the thread wait behind the whole burst, which shows up as
    // a p99 in the milliseconds while p50 stays in the microseconds. Capping the
    // batch and coming back for the rest keeps reads interleaved with writes.
    constexpr int kMaxPerPass = 32;
    int budget = kMaxPerPass;

    for (auto& s : shards_) {
        container::SpscByteRing& ring = s->outbox(t.index);
        for (;;) {
            if (budget-- <= 0) return true;
            const std::span<const std::byte> rec = ring.peek();
            if (rec.empty()) break;

            world::OutRecord hdr;
            std::memcpy(&hdr, rec.data(), sizeof hdr);
            const std::byte* frame = rec.data() + sizeof hdr;

            if (t.alive(hdr.conn_id)) {
                net::Connection* c = &t.conns[hdr.conn_id];
                // Slot reuse guard: by the time a delta reaches us, the client it
                // was built for may have disconnected and the slot been handed to
                // somebody else. The session in the frame header settles it.
                proto::FrameHeader fh;
                std::memcpy(&fh, frame, sizeof fh);
                if (c->session() == fh.session) {
                    net::IoOutcome out{};
                    // The bytes handed to write(2) are the bytes the shard wrote
                    // into the ring. No staging copy happens anywhere in between.
                    if (!c->send_now(frame, hdr.frame_bytes, out)) {
                        ++t.counters.evicted_slow;
                        close_connection(t, *c);
                    } else {
                        ++t.counters.frames_out;
                        t.counters.bytes_out += hdr.frame_bytes;
                        ++t.counters.write_calls;
                        if (out.result == net::IoResult::WouldBlock) ++t.counters.short_writes;
                        if (c->fd() >= 0 && c->has_pending() &&
                            !(t.interest[hdr.conn_id] & net::kWrite)) {
                            t.reactor.modify(c->fd(), hdr.conn_id,
                                             std::uint8_t(net::kRead | net::kWrite));
                            t.interest[hdr.conn_id] = std::uint8_t(net::kRead | net::kWrite);
                        }
                    }
                }
            }
            ring.pop();
        }
    }
    return false;
}

void WorldServer::close_connection(IoThread& t, net::Connection& c) {
    if (c.fd() < 0) return;
    if (c.handshaked()) {
        world::ShardCommand cmd{};
        cmd.kind      = world::ShardCommand::Kind::Leave;
        cmd.io_thread = t.index;
        cmd.conn_id   = c.id();
        cmd.session   = c.session();
        shards_[c.shard()]->inbox(t.index).try_push(cmd);
    }
    const std::uint32_t idx = c.id();
    t.reactor.remove(c.fd());
    t.interest[idx] = net::kNone;
    ::close(c.fd());
    // Reset in place and return the index to the free list. The slot keeps its
    // buffers and stays readable: a shard may still hold this index for a delta
    // already in flight, and it must read back fd == -1 rather than garbage.
    c.init(-1, idx, t.rbufs[idx], cfg_.read_buf_bytes, t.wbufs[idx], cfg_.write_buf_bytes);
    t.free_slots.push_back(idx);
    ++t.counters.closes;
    if (t.live.get()) t.live = t.live.get() - 1;
}

// ------------------------------------------------------------------ reporting
ServerReport WorldServer::report() const {
    ServerReport r{};

    metrics::Histogram ping{10'000'000ULL, 3}, loop{1'000'000'000ULL, 3};
    for (const auto& t : io_) {
        r.io.add(t->counters.load());
        ping.merge(t->ping_service);
        loop.merge(t->loop_work);
        r.live_connections += t->live;
        r.reserved_bytes   += t->buffers.capacity()
                            + t->conns.capacity() * sizeof(net::Connection);
    }

    metrics::Histogram jit{1'000'000'000ULL, 3}, dur{1'000'000'000ULL, 3}, tr{1'000'000'000ULL, 3};
    for (const auto& s : shards_) {
        r.shard.add(s->counters().load());
        jit.merge(s->tick_jitter());
        dur.merge(s->tick_duration());
        tr.merge(s->queue_transit());
        r.live_entities += s->live_entities();
        r.reserved_bytes += s->memory_bytes();
    }

    r.ping_service   = metrics::LatencySummary::of(ping);
    r.loop_work      = metrics::LatencySummary::of(loop);
    r.used_reuseport = use_reuseport_;
    r.io_backend     = io_.empty() ? "none" : io_.front()->reactor.backend_name();
    {
        std::string plan;
        bool any_pinned = false;
        for (const auto& t : io_) {
            if (t->cpu >= 0) any_pinned = true;
            plan += "io" + std::to_string(t->index) + "->" +
                    (t->cpu >= 0 ? std::to_string(t->cpu) : std::string("-")) + " ";
        }
        r.threads_pinned = any_pinned;
        r.placement = core::topology().describe() + (plan.empty() ? "" : ("; " + plan));
    }
    r.tick_jitter    = metrics::LatencySummary::of(jit);
    r.tick_duration  = metrics::LatencySummary::of(dur);
    r.queue_transit  = metrics::LatencySummary::of(tr);
    r.bytes_per_conn = cfg_.read_buf_bytes + cfg_.write_buf_bytes + sizeof(net::Connection);
    r.alloc_total    = alloc::snapshot();
    return r;
}

bool WorldServer::write_histogram_csv(const std::string& path) const {
    metrics::Histogram ping{10'000'000ULL, 3}, loop{1'000'000'000ULL, 3};
    metrics::Histogram jit{1'000'000'000ULL, 3}, dur{1'000'000'000ULL, 3}, tr{1'000'000'000ULL, 3};
    for (const auto& t : io_) { ping.merge(t->ping_service); loop.merge(t->loop_work); }
    for (const auto& s : shards_) {
        jit.merge(s->tick_jitter());
        dur.merge(s->tick_duration());
        tr.merge(s->queue_transit());
    }
    return bench::write_histogram_csv(path, {
        {"server_ping_service", &ping}, {"server_loop_work", &loop},
        {"shard_tick_jitter", &jit},    {"shard_tick_duration", &dur},
        {"io_to_shard_transit", &tr},
    });
}

void WorldServer::reset_measurements() {
    for (auto& t : io_) {
        t->ping_service.reset();
        t->loop_work.reset();
        t->counters = metrics::IoCounters{};
    }
}

} // namespace ge::server
