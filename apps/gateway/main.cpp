// ge_gateway -- front door that spreads client sessions across world servers.
//
// Forwarding is header-only: the gateway parses the 16-byte frame header to
// learn the session and to count traffic, then forwards the *entire contiguous
// run of complete frames* in a single write. It never touches a payload, never
// re-encodes, and never stages bytes in a second buffer -- the memory handed to
// write(2) is the memory read(2) filled.
//
// Each proxy thread owns its own Balancer. Sharing one would mean an atomic
// read-modify-write on every connection and a shared cache line on the hot
// path, to synchronise state that is statistical anyway: two threads with
// independent views of backend load converge on the same distribution.
#include <atomic>
#include <cstdint>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "ge/alloc/arena.hpp"
#include "ge/bench/report.hpp"
#include "ge/alloc/pool.hpp"
#include "ge/container/mpmc_queue.hpp"
#include "ge/core/clock.hpp"
#include "ge/core/thread.hpp"
#include "ge/lb/balancer.hpp"
#include "ge/metrics/histogram.hpp"
#include "ge/metrics/process.hpp"
#include "ge/net/connection.hpp"
#include "ge/net/reactor.hpp"
#include "ge/net/socket.hpp"
#include "ge/proto/codec.hpp"

namespace {

using ge::core::Clock;
namespace net   = ge::net;
namespace proto = ge::proto;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_release); }

struct GatewayConfig {
    std::string   host = "127.0.0.1";
    std::uint16_t port = 8000;
    std::uint16_t threads = 2;
    std::uint32_t max_pairs = 512;          // per thread
    std::uint32_t buf_bytes = 16384;        // power of two
    std::int64_t  probe_interval_ns = 1'000'000'000;
    std::int64_t  probe_timeout_ns  = 2'000'000'000;
    std::vector<ge::lb::BackendConfig> backends;
};

// Token layout: the top 2 bits say what kind of fd this is, the rest is an
// index. One reactor, three kinds of descriptor, no lookup table.
constexpr std::uint64_t kKindShift  = 62;
constexpr std::uint64_t kKindClient = 0;
constexpr std::uint64_t kKindServer = 1;
constexpr std::uint64_t kKindProbe  = 2;
constexpr std::uint64_t kIndexMask  = (std::uint64_t{1} << kKindShift) - 1;

constexpr std::uint64_t make_token(std::uint64_t kind, std::uint64_t idx) {
    return (kind << kKindShift) | idx;
}

struct Pair {
    net::Connection client;
    net::Connection backend;
    int             backend_idx = -1;
    bool            active      = false;
    std::uint32_t   session     = 0;
};

struct ProbeConn {
    int          fd        = -1;
    std::byte*   rbuf      = nullptr;
    std::uint32_t rlen     = 0;
    std::int64_t sent_ns   = 0;
    bool         awaiting  = false;
};

struct Proxy {
    std::uint16_t             index = 0;
    net::Reactor              reactor;
    ge::lb::Balancer          balancer;
    ge::alloc::Arena          buffers;
    std::vector<Pair>         pairs;
    std::vector<ProbeConn>    probes;
    ge::container::MpmcQueue<int> handoff{256};

    ge::metrics::Histogram forward_ns{10'000'000ULL, 3};
    std::uint64_t frames_c2s = 0, frames_s2c = 0;
    std::uint64_t bytes_c2s = 0, bytes_s2c = 0;
    std::uint64_t sessions = 0, rejected = 0, backend_failures = 0;
    std::int64_t  next_probe_ns = 0;
};

// ----------------------------------------------------------------- plumbing
void teardown(Proxy& p, std::uint32_t idx) {
    Pair& pr = p.pairs[idx];
    if (!pr.active) return;
    if (pr.client.fd() >= 0)  { p.reactor.remove(pr.client.fd());  ::close(pr.client.fd()); }
    if (pr.backend.fd() >= 0) { p.reactor.remove(pr.backend.fd()); ::close(pr.backend.fd()); }
    if (pr.backend_idx >= 0) p.balancer.on_session_closed(pr.backend_idx);
    pr.client.init(-1, idx, nullptr, 0, nullptr, 1);
    pr.backend.init(-1, idx, nullptr, 0, nullptr, 1);
    pr.active = false;
    pr.backend_idx = -1;
}

// Reads from `from`, parses frame headers for accounting, and forwards every
// complete frame in one write. Returns false when the pair must be torn down.
bool pump(Proxy& p, net::Connection& from, net::Connection& to,
          std::uint64_t& frame_counter, std::uint64_t& byte_counter,
          std::uint32_t* session_out) {
    const net::IoOutcome r = from.fill();
    if (r.result == net::IoResult::Closed || r.result == net::IoResult::Error) return false;

    const std::int64_t t0 = Clock::now_ns();
    proto::FrameReader reader(from.readable());
    proto::FrameView   f;
    std::uint64_t frames = 0;
    for (;;) {
        const proto::ParseError e = reader.next(f);
        if (e == proto::ParseError::Incomplete) break;
        if (e != proto::ParseError::Ok) return false;     // desynced stream: drop the pair
        ++frames;
        if (session_out && f.session() != 0) *session_out = f.session();
    }
    const std::size_t n = reader.consumed();
    if (n == 0) return r.result != net::IoResult::Closed;

    // One write for every complete frame that arrived in this read. Forwarding
    // per frame would multiply the syscall count by the batch size for no gain.
    net::IoOutcome out{};
    if (!to.send_now(from.readable().data(), std::uint32_t(n), out)) return false;
    from.consume(n);

    frame_counter += frames;
    byte_counter  += n;
    p.forward_ns.record(std::uint64_t(Clock::now_ns() - t0));
    return true;
}

void send_probes(Proxy& p, const GatewayConfig& cfg, std::int64_t now) {
    for (std::size_t i = 0; i < p.probes.size(); ++i) {
        ProbeConn& pc = p.probes[i];
        if (pc.fd < 0) {
            auto fd = net::connect_tcp(cfg.backends[i].host.c_str(), cfg.backends[i].port, false);
            if (!fd) { p.balancer.on_connect_failed(int(i), now); continue; }
            net::set_nonblocking(fd->get());
            net::set_nodelay(fd->get());
            pc.fd = fd->release();
            p.reactor.add(pc.fd, make_token(kKindProbe, i), net::kRead);
        }
        // An outstanding probe that never came back is a timeout, not a reason
        // to pile on another probe: one in flight at a time per backend.
        if (pc.awaiting) {
            if (now - pc.sent_ns > cfg.probe_timeout_ns) {
                p.balancer.on_probe_timeout(int(i), now);
                pc.awaiting = false;
            }
            continue;
        }
        alignas(8) std::byte buf[sizeof(proto::FrameHeader) + sizeof(proto::HealthProbe)];
        const proto::HealthProbe hp{std::uint64_t(now), 0};
        const std::size_t n = proto::encode(buf, hp, 0, 0);
        if (net::write_some(pc.fd, buf, n).result != net::IoResult::Ok) {
            p.balancer.on_probe_timeout(int(i), now);
            p.reactor.remove(pc.fd);
            ::close(pc.fd);
            pc.fd = -1;
            continue;
        }
        pc.sent_ns = now;
        pc.awaiting = true;
        p.balancer.on_probe_sent(int(i));
    }
}

void handle_probe_reply(Proxy& p, std::size_t i) {
    ProbeConn& pc = p.probes[i];
    const net::IoOutcome r = net::read_some(pc.fd, pc.rbuf + pc.rlen, 4096 - pc.rlen);
    if (r.result == net::IoResult::Closed || r.result == net::IoResult::Error) {
        p.reactor.remove(pc.fd);
        ::close(pc.fd);
        pc.fd = -1; pc.rlen = 0; pc.awaiting = false;
        p.balancer.on_probe_timeout(int(i), Clock::now_ns());
        return;
    }
    if (r.result == net::IoResult::Ok) pc.rlen += std::uint32_t(r.bytes);

    proto::FrameReader reader(std::span<const std::byte>(pc.rbuf, pc.rlen));
    proto::FrameView f;
    while (reader.next(f) == proto::ParseError::Ok) {
        if (const auto* rep = f.as<proto::HealthReply>()) {
            p.balancer.on_probe_reply(int(i),
                                      Clock::now_ns() - std::int64_t(rep->probe_ts_ns),
                                      rep->inflight_pct);
            pc.awaiting = false;
        }
    }
    const std::size_t used = reader.consumed();
    if (used) { pc.rlen -= std::uint32_t(used); if (pc.rlen) std::memmove(pc.rbuf, pc.rbuf + used, pc.rlen); }
}

void proxy_loop(Proxy& p, const GatewayConfig& cfg) {
    char name[32];
    std::snprintf(name, sizeof name, "ge-gw-%u", unsigned(p.index));
    ge::core::set_thread_name(name);
    ge::core::request_perf_core(true);

    net::Event evs[256];
    while (!g_stop.load(std::memory_order_relaxed)) {
        const std::int64_t now = Clock::now_ns();

        // Adopt new clients and open the matching backend connection.
        int fd;
        while (p.handoff.try_pop(fd)) {
            std::uint32_t slot = UINT32_MAX;
            for (std::uint32_t i = 0; i < p.pairs.size(); ++i)
                if (!p.pairs[i].active) { slot = i; break; }
            if (slot == UINT32_MAX) { ::close(fd); ++p.rejected; continue; }

            // Affinity key is not known until the first frame arrives, so the
            // initial placement is load-based; session stickiness applies to
            // reconnects, which is when it actually matters.
            const int be = p.balancer.pick(0, now);
            if (be < 0) { ::close(fd); ++p.rejected; continue; }

            auto bfd = net::connect_tcp(cfg.backends[std::size_t(be)].host.c_str(),
                                        cfg.backends[std::size_t(be)].port, false);
            if (!bfd) {
                p.balancer.on_connect_failed(be, now);
                ++p.backend_failures;
                ::close(fd);
                continue;
            }
            net::set_nonblocking(fd);
            net::set_nonblocking(bfd->get());
            net::set_nodelay(bfd->get());

            Pair& pr = p.pairs[slot];
            auto* cr = static_cast<std::byte*>(p.buffers.allocate(cfg.buf_bytes, 8));
            auto* cw = static_cast<std::byte*>(p.buffers.allocate(cfg.buf_bytes, 8));
            auto* br = static_cast<std::byte*>(p.buffers.allocate(cfg.buf_bytes, 8));
            auto* bw = static_cast<std::byte*>(p.buffers.allocate(cfg.buf_bytes, 8));
            if (!cr || !cw || !br || !bw) { ::close(fd); ++p.rejected; continue; }

            pr.client.init(fd, slot, cr, cfg.buf_bytes, cw, cfg.buf_bytes);
            pr.backend.init(bfd->release(), slot, br, cfg.buf_bytes, bw, cfg.buf_bytes);
            pr.backend_idx = be;
            pr.active = true;
            p.reactor.add(pr.client.fd(),  make_token(kKindClient, slot), net::kRead);
            p.reactor.add(pr.backend.fd(), make_token(kKindServer, slot), net::kRead);
            p.balancer.on_session_opened(be);
            ++p.sessions;
        }

        if (now >= p.next_probe_ns) {
            p.balancer.tick(now);
            send_probes(p, cfg, now);
            p.next_probe_ns = now + cfg.probe_interval_ns;
        }

        const int n = p.reactor.poll(evs, 256, 50);
        if (n < 0) break;
        for (int i = 0; i < n; ++i) {
            const std::uint64_t tok = evs[i].token;
            if (tok == net::kWakeToken) continue;
            const std::uint64_t kind = tok >> kKindShift;
            const auto idx = std::uint32_t(tok & kIndexMask);

            if (kind == kKindProbe) { if (p.probes[idx].fd >= 0) handle_probe_reply(p, idx); continue; }
            if (idx >= p.pairs.size() || !p.pairs[idx].active) continue;
            Pair& pr = p.pairs[idx];

            bool ok = true;
            if (evs[i].writable) {
                net::Connection& c = (kind == kKindClient) ? pr.client : pr.backend;
                const net::IoOutcome w = c.flush();
                ok = !(w.result == net::IoResult::Closed || w.result == net::IoResult::Error);
            }
            if (ok && evs[i].readable) {
                ok = (kind == kKindClient)
                   ? pump(p, pr.client, pr.backend, p.frames_c2s, p.bytes_c2s, &pr.session)
                   : pump(p, pr.backend, pr.client, p.frames_s2c, p.bytes_s2c, nullptr);
            }
            if (!ok || (evs[i].hangup && !pr.client.has_pending() && !pr.backend.has_pending())) {
                teardown(p, idx);
                continue;
            }
            // Re-arm write interest on whichever side has queued output.
            p.reactor.modify(pr.client.fd(), make_token(kKindClient, idx),
                             std::uint8_t(net::kRead | (pr.client.has_pending() ? net::kWrite : 0)));
            p.reactor.modify(pr.backend.fd(), make_token(kKindServer, idx),
                             std::uint8_t(net::kRead | (pr.backend.has_pending() ? net::kWrite : 0)));
        }
    }
    for (std::uint32_t i = 0; i < p.pairs.size(); ++i) teardown(p, i);
}

[[noreturn]] void usage(const char* a0) {
    std::printf(
"usage: %s --backend HOST:PORT [--backend HOST:PORT ...] [options]\n"
"    --host H          bind address        (default 127.0.0.1)\n"
"    --port P          bind port           (default 8000)\n"
"    --threads N       proxy threads       (default 2)\n"
"    --max-pairs N     sessions per thread (default 512)\n"
"    --probe-ms N      health probe interval in ms (default 1000)\n", a0);
    std::exit(2);
}

} // namespace

int main(int argc, char** argv) {
    GatewayConfig cfg;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { if (i + 1 >= argc) usage(argv[0]); return argv[++i]; };
        if (!std::strcmp(argv[i], "--backend")) {
            const std::string spec = next();
            const auto colon = spec.rfind(':');
            if (colon == std::string::npos) usage(argv[0]);
            cfg.backends.push_back({spec.substr(0, colon),
                                    std::uint16_t(std::atoi(spec.c_str() + colon + 1)), 1});
        }
        else if (!std::strcmp(argv[i], "--host"))      cfg.host = next();
        else if (!std::strcmp(argv[i], "--port"))      cfg.port = std::uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--threads"))   cfg.threads = std::uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--max-pairs")) cfg.max_pairs = std::uint32_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--probe-ms"))  cfg.probe_interval_ns = std::atoll(next()) * 1'000'000;
        else usage(argv[0]);
    }
    if (cfg.backends.empty()) usage(argv[0]);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    auto listener = net::listen_tcp(cfg.host.c_str(), cfg.port, 1024);
    if (!listener) { std::fprintf(stderr, "listen: %s\n", listener.error().c_str()); return 1; }
    net::Fd listen_fd = std::move(*listener);

    std::vector<std::unique_ptr<Proxy>> proxies;
    for (std::uint16_t i = 0; i < cfg.threads; ++i) {
        auto p = std::make_unique<Proxy>();
        p->index = i;
        if (auto r = p->reactor.open(); !r) {
            std::fprintf(stderr, "reactor: %s\n", r.error().c_str());
            return 1;
        }
        p->balancer.configure(cfg.backends, ge::lb::BalancerConfig{});
        // Four buffers per pair (client r/w, backend r/w) plus the probe buffers.
        p->buffers.reserve(std::size_t(cfg.max_pairs) * cfg.buf_bytes * 4
                           + cfg.backends.size() * 4096 + 4096);
        p->buffers.prefault();
        p->pairs.resize(cfg.max_pairs);
        for (std::uint32_t s = 0; s < cfg.max_pairs; ++s) {
            p->pairs[s].client.init(-1, s, nullptr, 0, nullptr, 1);
            p->pairs[s].backend.init(-1, s, nullptr, 0, nullptr, 1);
        }
        p->probes.resize(cfg.backends.size());
        for (auto& pc : p->probes) pc.rbuf = static_cast<std::byte*>(p->buffers.allocate(4096, 8));
        proxies.push_back(std::move(p));
    }

    std::vector<std::thread> threads;
    for (auto& p : proxies) threads.emplace_back([&, q = p.get()] { proxy_loop(*q, cfg); });

    std::printf("ge_gateway listening on %s:%u -- %zu backends, %u proxy threads\n",
                cfg.host.c_str(), net::local_port(listen_fd.get()), cfg.backends.size(), cfg.threads);
    for (std::size_t i = 0; i < cfg.backends.size(); ++i)
        std::printf("  backend[%zu] %s:%u\n", i, cfg.backends[i].host.c_str(), cfg.backends[i].port);
    std::fflush(stdout);

    // Acceptor runs on the main thread.
    net::Reactor acc;
    if (!acc.open()) return 1;
    acc.add(listen_fd.get(), 0, net::kRead);
    net::Event evs[64];
    std::uint32_t rr = 0;
    while (!g_stop.load(std::memory_order_acquire)) {
        const int n = acc.poll(evs, 64, 200);
        if (n < 0) break;
        for (int i = 0; i < n; ++i) {
            if (evs[i].token == net::kWakeToken) continue;
            for (;;) {
                const int fd = ::accept(listen_fd.get(), nullptr, nullptr);
                if (fd < 0) break;
                net::set_nodelay(fd);
                net::set_nosigpipe(fd);
                Proxy& p = *proxies[rr++ % proxies.size()];
                if (!p.handoff.try_push(fd)) { ::close(fd); continue; }
                p.reactor.wake();
            }
        }
    }

    g_stop.store(true, std::memory_order_release);
    for (auto& p : proxies) p->reactor.wake();
    for (auto& t : threads) t.join();

    // ------------------------------------------------------------- summary
    std::printf("\nGATEWAY SUMMARY\n");
    std::printf("--------------------------------------------------------------------------\n");
    ge::metrics::Histogram fwd{10'000'000ULL, 3};
    std::uint64_t c2s = 0, s2c = 0, sess = 0, rej = 0, bf = 0, bc2s = 0, bs2c = 0;
    for (auto& p : proxies) {
        fwd.merge(p->forward_ns);
        c2s += p->frames_c2s; s2c += p->frames_s2c;
        bc2s += p->bytes_c2s; bs2c += p->bytes_s2c;
        sess += p->sessions;  rej += p->rejected; bf += p->backend_failures;
    }
    std::printf("  sessions proxied      %llu   (rejected %llu, backend connect failures %llu)\n",
                static_cast<unsigned long long>(sess), static_cast<unsigned long long>(rej), static_cast<unsigned long long>(bf));
    std::printf("  frames forwarded      %llu client->backend, %llu backend->client\n",
                static_cast<unsigned long long>(c2s), static_cast<unsigned long long>(s2c));
    std::printf("  bytes forwarded       %s up, %s down\n",
                ge::metrics::format_bytes(bc2s).c_str(), ge::metrics::format_bytes(bs2c).c_str());
    std::printf("\n");
    ge::bench::print_latency("gateway added latency", ge::metrics::LatencySummary::of(fwd),
        "parse headers + forward one batch. This is the cost of the extra hop.");

    std::printf("  per-backend routing (thread 0's view)\n");
    const auto& b0 = proxies[0]->balancer;
    const auto share = b0.key_space_share();
    for (std::size_t i = 0; i < cfg.backends.size(); ++i) {
        const auto& s = b0.stats(int(i));
        std::printf("    [%zu] %-22s sessions %-6llu inflight %-5llu rtt(ewma) %-8.3f ms  "
                    "probes %llu/%llu  ejections %llu  hash-share %.1f%%  %s\n",
                    i, (cfg.backends[i].host + ":" + std::to_string(cfg.backends[i].port)).c_str(),
                    static_cast<unsigned long long>(s.sessions_assigned), static_cast<unsigned long long>(s.inflight),
                    double(s.rtt_ewma_ns) / 1e6,
                    static_cast<unsigned long long>(s.probes_ok), static_cast<unsigned long long>(s.probes_sent),
                    static_cast<unsigned long long>(s.ejections), share[i] * 100.0,
                    s.healthy ? "healthy" : "EJECTED");
    }
    return 0;
}
