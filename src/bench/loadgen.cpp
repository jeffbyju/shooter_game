#include "ge/bench/loadgen.hpp"
#include <cstdio>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <cmath>
#include <cstring>
#include <thread>
#include <unistd.h>

#include "ge/alloc/arena.hpp"
#include "ge/bench/report.hpp"
#include "ge/core/bits.hpp"
#include "ge/core/clock.hpp"
#include "ge/core/thread.hpp"
#include "ge/net/reactor.hpp"
#include "ge/net/socket.hpp"
#include "ge/proto/codec.hpp"

namespace ge::bench {
namespace {

using core::Clock;

struct Client {
    int           fd          = -1;
    std::byte*    rbuf        = nullptr;
    std::uint32_t rcap        = 0;
    std::uint32_t rlen        = 0;
    std::uint32_t session     = 0;
    std::uint32_t input_seq   = 0;
    std::uint64_t last_echo   = 0;   // dedupes repeated echoes across ticks
    bool          handshaked  = false;
    bool          dead        = false;
};

struct Worker {
    std::uint32_t        index = 0;
    net::Reactor         reactor;
    std::vector<Client>  clients;
    alloc::Arena         buffers;
    core::Xoshiro256ss   rng{1};

    metrics::Histogram ping_sched{60'000'000'000ULL, 3};
    metrics::Histogram ping_wire{60'000'000'000ULL, 3};
    metrics::Histogram e2e{60'000'000'000ULL, 3};
    metrics::Histogram send_delay{60'000'000'000ULL, 3};
    metrics::Histogram upstream{60'000'000'000ULL, 3};
    metrics::Histogram downstream{60'000'000'000ULL, 3};

    std::uint64_t pings_sent = 0, pongs = 0, moves_sent = 0, deltas = 0, entities = 0;
    std::uint64_t bytes_in = 0, bytes_out = 0, parse_errors = 0, disconnects = 0;
    std::uint64_t send_blocked = 0, connected = 0;

    // Every outbound frame is built here: one aligned stack-lifetime buffer per
    // worker, reused for the whole run, so the generator itself allocates
    // nothing once it is running.
    alignas(8) std::byte scratch[128];
};

constexpr std::uint32_t kRecvBuf = 1u << 16;

void reset_histograms(Worker& w) {
    w.ping_sched.reset(); w.ping_wire.reset(); w.e2e.reset(); w.send_delay.reset();
    w.upstream.reset(); w.downstream.reset();
    w.pings_sent = w.pongs = w.moves_sent = w.deltas = w.entities = 0;
    w.bytes_in = w.bytes_out = 0;
    w.send_blocked = 0;
}

bool send_frame(Worker& w, Client& c, const void* buf, std::size_t n) {
    const net::IoOutcome r = net::write_some(c.fd, buf, n);
    if (r.result == net::IoResult::Ok && std::size_t(r.bytes) == n) {
        w.bytes_out += n;
        return true;
    }
    if (r.result == net::IoResult::WouldBlock) { ++w.send_blocked; return false; }
    if (r.result == net::IoResult::Ok) { w.bytes_out += std::size_t(r.bytes); ++w.send_blocked; return false; }
    c.dead = true;
    ++w.disconnects;
    return false;
}

// Parses everything currently buffered on one client and updates the histograms.
void drain_client(Worker& w, Client& c, std::int64_t now) {
    for (;;) {
        if (c.rlen >= c.rcap) { c.dead = true; ++w.parse_errors; return; }
        const net::IoOutcome r = net::read_some(c.fd, c.rbuf + c.rlen, c.rcap - c.rlen);
        if (r.result == net::IoResult::Closed || r.result == net::IoResult::Error) {
            c.dead = true; ++w.disconnects; return;
        }
        if (r.result == net::IoResult::Ok) { c.rlen += std::uint32_t(r.bytes); w.bytes_in += std::uint64_t(r.bytes); }

        proto::FrameReader reader(std::span<const std::byte>(c.rbuf, c.rlen));
        proto::FrameView   f;
        for (;;) {
            const proto::ParseError e = reader.next(f);
            if (e == proto::ParseError::Incomplete) break;
            if (e != proto::ParseError::Ok) { ++w.parse_errors; c.dead = true; return; }

            switch (f.type()) {
                case proto::MsgType::ServerWelcome: {
                    if (const auto* m = f.as<proto::ServerWelcome>()) {
                        c.session = m->session_id;
                        if (!c.handshaked) { c.handshaked = true; ++w.connected; }
                    }
                    break;
                }
                case proto::MsgType::Pong: {
                    const auto* m = f.as<proto::Pong>();
                    if (!m) break;
                    // client_ts_ns is when this request was *due*; cookie is when
                    // it actually left. The first is the user-visible latency,
                    // the second isolates the server from client-side lateness.
                    const auto sched = std::int64_t(m->client_ts_ns);
                    const auto wire  = std::int64_t(m->cookie);
                    if (sched > 0 && now >= sched) w.ping_sched.record(std::uint64_t(now - sched));
                    if (wire  > 0 && now >= wire)  w.ping_wire.record(std::uint64_t(now - wire));
                    // Splits the round trip at the server's own receive stamp.
                    const auto srv = std::int64_t(m->server_recv_ns);
                    if (srv >= wire && now >= srv) {
                        w.upstream.record(std::uint64_t(srv - wire));
                        w.downstream.record(std::uint64_t(now - srv));
                    }
                    ++w.pongs;
                    break;
                }
                case proto::MsgType::SnakeDelta: {
                    const auto* m = f.as<proto::SnakeDeltaHeader>();
                    if (!m) break;
                    ++w.deltas;
                    w.entities += m->count;
                    // A delta goes out every tick carrying the newest input it
                    // has folded in. Recording every one would count the same
                    // input many times and drag the distribution toward the tick
                    // period; only the first delta that reflects a given input
                    // is a measurement of that input's latency.
                    if (m->echo_client_ts_ns != 0 && m->echo_client_ts_ns != c.last_echo) {
                        c.last_echo = m->echo_client_ts_ns;
                        const auto t0 = std::int64_t(m->echo_client_ts_ns);
                        if (now >= t0) w.e2e.record(std::uint64_t(now - t0));
                    }
                    break;
                }
                default: break;
            }
        }
        const std::size_t used = reader.consumed();
        if (used) {
            c.rlen -= std::uint32_t(used);
            if (c.rlen) std::memmove(c.rbuf, c.rbuf + used, c.rlen);
        }
        if (r.result == net::IoResult::WouldBlock || r.bytes == 0) return;
    }
}

core::Status connect_all(Worker& w, const LoadConfig& cfg) {
    for (auto& c : w.clients) {
        auto fd = net::connect_tcp(cfg.host.c_str(), cfg.port, false);
        if (!fd) return std::unexpected(fd.error());
        net::set_nodelay(fd->get());
        net::set_nosigpipe(fd->get());
        net::set_nonblocking(fd->get());
        c.fd = fd->release();

        alignas(8) std::byte buf[sizeof(proto::FrameHeader) + sizeof(proto::ClientHello)];
        const proto::ClientHello hello{w.rng.next(), proto::kProtocolVersion, 0};
        const std::size_t n = proto::encode(buf, hello, 0, 0);
        // Handshake writes are blocking-equivalent: the socket is fresh and the
        // frame is 32 bytes, so a partial write here means something is wrong.
        if (!send_frame(w, c, buf, n)) return core::fail("hello write failed");
    }

    const std::int64_t deadline = Clock::now_ns() + std::int64_t(cfg.connect_timeout_s * 1e9);
    while (w.connected < w.clients.size() && Clock::now_ns() < deadline) {
        for (auto& c : w.clients) {
            if (!c.dead && !c.handshaked) drain_client(w, c, Clock::now_ns());
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (w.connected < w.clients.size())
        return core::fail("handshake timeout",
                          std::to_string(w.connected) + "/" +
                          std::to_string(w.clients.size()) + " clients connected");
    return core::ok();
}

void run_worker(Worker& w, const LoadConfig& cfg,
                std::atomic<std::uint32_t>& ready, std::atomic<std::int64_t>& origin,
                std::atomic<bool>& failed, core::Error* err_out, std::mutex& err_mu) {
    char name[32];
    std::snprintf(name, sizeof name, "ge-load-%u", unsigned(w.index));
    core::set_thread_name(name);
    core::request_perf_core(true);

    if (auto r = connect_all(w, cfg); !r) {
        failed.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> g(err_mu);
        if (err_out && err_out->message.empty()) *err_out = r.error();
        ready.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    // Rendezvous: every worker starts its schedule from the same origin, so the
    // offered load is a single coherent arrival process rather than N unrelated
    // ones that happen to overlap.
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (origin.load(std::memory_order_acquire) == 0) {
        if (failed.load(std::memory_order_acquire)) return;
        core::cpu_relax();
    }
    const std::int64_t t_origin = origin.load(std::memory_order_acquire);

    const auto n_clients   = double(w.clients.size());
    const double ping_iv   = cfg.ping_rate_hz > 0 ? 1e9 / (n_clients * cfg.ping_rate_hz) : 0;
    const double move_iv   = cfg.move_rate_hz > 0 ? 1e9 / (n_clients * cfg.move_rate_hz) : 0;

    auto next_gap = [&](double mean) -> double {
        if (mean <= 0) return 0;
        if (!cfg.poisson) return mean;
        // Exponential inter-arrivals: a Poisson process is a far better model of
        // aggregated independent clients than a metronome, and it exercises the
        // queueing behaviour that a uniform schedule never reaches.
        const double u = std::max(1e-12, w.rng.next_double());
        return -mean * std::log(u);
    };

    std::int64_t ping_due = t_origin + std::int64_t(next_gap(ping_iv));
    std::int64_t move_due = t_origin + std::int64_t(next_gap(move_iv));
    const std::int64_t warmup_end = t_origin + std::int64_t(cfg.warmup_s * 1e9);
    const std::int64_t run_end    = warmup_end + std::int64_t(cfg.duration_s * 1e9);

    std::uint32_t ping_rr = 0, move_rr = 0;
    bool warm = false;

    net::Event evs[256];
    for (auto& c : w.clients) w.reactor.add(c.fd, std::uint64_t(&c - w.clients.data()), net::kRead);

    while (Clock::now_ns() < run_end) {
        const std::int64_t now = Clock::now_ns();

        if (!warm && now >= warmup_end) { reset_histograms(w); warm = true; }

        // ---- issue everything the schedule says is due ----------------------
        int budget = 512;   // bounded so reads still get serviced under overload
        while (ping_iv > 0 && ping_due <= now && budget-- > 0) {
            Client& c = w.clients[ping_rr++ % w.clients.size()];
            if (!c.dead) {
                const std::int64_t actual = Clock::now_ns();
                w.send_delay.record(std::uint64_t(std::max<std::int64_t>(0, actual - ping_due)));
                const proto::Ping p{std::uint64_t(ping_due), std::uint64_t(actual)};
                const std::size_t n = proto::encode(w.scratch, p, c.session, c.input_seq++);
                if (send_frame(w, c, w.scratch, n)) ++w.pings_sent;
            }
            ping_due += std::int64_t(next_gap(ping_iv));
        }
        budget = 512;
        while (move_iv > 0 && move_due <= now && budget-- > 0) {
            Client& c = w.clients[move_rr++ % w.clients.size()];
            if (!c.dead) {
                const float ang = float(w.rng.next_double() * 6.2831853);
                proto::MoveCommand m{};
                m.client_ts_ns = std::uint64_t(move_due);
                m.input_seq    = c.input_seq++;
                m.dx = std::cos(ang) * 40.0f;
                m.dy = std::sin(ang) * 40.0f;
                const std::size_t n = proto::encode(w.scratch, m, c.session, m.input_seq);
                if (send_frame(w, c, w.scratch, n)) ++w.moves_sent;
            }
            move_due += std::int64_t(next_gap(move_iv));
        }

        // ---- service replies ------------------------------------------------
        std::int64_t next_due = run_end;
        if (ping_iv > 0) next_due = std::min(next_due, ping_due);
        if (move_iv > 0) next_due = std::min(next_due, move_due);
        const std::int64_t slack_us = (next_due - Clock::now_ns()) / 1000;
        const int timeout_ms = slack_us <= 0 ? 0
                             : int(std::min<std::int64_t>(slack_us / 1000, cfg.poll_timeout_us / 1000 + 1));

        const int n = w.reactor.poll(evs, 256, timeout_ms);
        if (n < 0) break;
        const std::int64_t rx_now = Clock::now_ns();
        for (int i = 0; i < n; ++i) {
            if (evs[i].token == net::kWakeToken) continue;
            const auto idx = std::size_t(evs[i].token);
            if (idx >= w.clients.size()) continue;
            Client& c = w.clients[idx];
            if (!c.dead) drain_client(w, c, rx_now);
        }
    }

    for (auto& c : w.clients) if (c.fd >= 0) ::close(c.fd);
}

} // namespace

core::Result<LoadReport> run_load(const LoadConfig& cfg) {
    LoadReport rep{};
    const std::uint32_t nthreads = std::max(1u, cfg.threads);
    const std::uint32_t nclients = std::max(nthreads, cfg.clients);

    std::vector<std::unique_ptr<Worker>> workers;
    workers.reserve(nthreads);
    for (std::uint32_t i = 0; i < nthreads; ++i) {
        auto w = std::make_unique<Worker>();
        w->index = i;
        w->rng   = core::Xoshiro256ss(cfg.seed + i);
        if (auto r = w->reactor.open(); !r) return std::unexpected(r.error());

        const std::uint32_t share = nclients / nthreads + (i < nclients % nthreads ? 1 : 0);
        w->buffers.reserve(std::size_t(share) * kRecvBuf + 4096);
        w->buffers.prefault();
        w->clients.resize(share);
        for (auto& c : w->clients) {
            c.rbuf = static_cast<std::byte*>(w->buffers.allocate(kRecvBuf, 8));
            c.rcap = kRecvBuf;
        }
        workers.push_back(std::move(w));
    }

    std::atomic<std::uint32_t> ready{0};
    std::atomic<std::int64_t>  origin{0};
    std::atomic<bool>          failed{false};
    std::mutex                 err_mu;
    core::Error                err_obj;

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (auto& w : workers)
        threads.emplace_back([&, p = w.get()] { run_worker(*p, cfg, ready, origin, failed, &err_obj, err_mu); });

    while (ready.load(std::memory_order_acquire) < nthreads)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    if (failed.load(std::memory_order_acquire)) {
        origin.store(Clock::now_ns(), std::memory_order_release);
        for (auto& t : threads) t.join();
        return core::fail(err_obj.message.empty() ? "load generator failed to start"
                                                 : err_obj.message);
    }

    // Resource accounting starts after warmup would have, so connect-time
    // allocations and page faults do not pollute the steady-state numbers.
    const std::int64_t t0 = Clock::now_ns();
    origin.store(t0, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.warmup_s));

    const metrics::ProcessSample before = metrics::sample_process();
    const alloc::AllocScope      alloc_scope;
    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.duration_s));
    const metrics::ProcessSample after = metrics::sample_process();

    // Close the allocation window *here*, before anything below runs. The
    // aggregation histograms constructed a few lines down allocate, and an
    // earlier version of this function read the delta after building them --
    // so the harness reported its own reporting path as six allocations on the
    // hot path. Measuring instruments have to exclude themselves.
    const alloc::AllocStats steady_alloc = alloc_scope.delta();

    for (auto& t : threads) t.join();

    metrics::Histogram ps{60'000'000'000ULL, 3}, pw{60'000'000'000ULL, 3},
                       e2e{60'000'000'000ULL, 3}, sd{60'000'000'000ULL, 3},
                       up{60'000'000'000ULL, 3}, down{60'000'000'000ULL, 3};
    for (auto& w : workers) {
        ps.merge(w->ping_sched); pw.merge(w->ping_wire);
        e2e.merge(w->e2e);       sd.merge(w->send_delay);
        up.merge(w->upstream);   down.merge(w->downstream);
        rep.clients_connected += w->connected;
        rep.pings_sent   += w->pings_sent;
        rep.pongs_received += w->pongs;
        rep.moves_sent   += w->moves_sent;
        rep.deltas_received += w->deltas;
        rep.entities_received += w->entities;
        rep.bytes_sent   += w->bytes_out;
        rep.bytes_received += w->bytes_in;
        rep.parse_errors += w->parse_errors;
        rep.disconnects  += w->disconnects;
    }

    rep.ping_scheduled = metrics::LatencySummary::of(ps);
    rep.ping_wire      = metrics::LatencySummary::of(pw);
    rep.e2e_scheduled  = metrics::LatencySummary::of(e2e);
    rep.send_delay     = metrics::LatencySummary::of(sd);
    rep.upstream       = metrics::LatencySummary::of(up);
    rep.downstream     = metrics::LatencySummary::of(down);

    rep.measured_seconds    = cfg.duration_s;
    rep.offered_msgs_per_s  = double(nclients) * (cfg.ping_rate_hz + cfg.move_rate_hz);
    rep.achieved_msgs_per_s = double(rep.pings_sent + rep.moves_sent) / cfg.duration_s;
    rep.reply_msgs_per_s    = double(rep.pongs_received + rep.deltas_received) / cfg.duration_s;

    if (!cfg.histogram_csv_path.empty()) {
        write_histogram_csv(cfg.histogram_csv_path, {
            {"ping_scheduled", &ps}, {"ping_wire", &pw}, {"input_to_state", &e2e},
            {"upstream", &up}, {"downstream", &down}, {"generator_send_delay", &sd},
        });
    }

    rep.client_resources    = metrics::diff(before, after);
    rep.client_alloc_steady = steady_alloc;
    rep.client_saturated    = rep.client_resources.cores_used() >= 0.9 * double(nthreads);
    return rep;
}

} // namespace ge::bench
