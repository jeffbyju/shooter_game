#pragma once
// The client's half of the protocol.
//
// Deliberately uses the same ge::proto codec the server does, so the wire
// format is defined in exactly one place. If this file and the server ever
// disagree, it is a bug in one of them rather than two copies of a spec that
// drifted.
#include <cstring>
#include <string>
#include <vector>

#include "ge/core/clock.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

#include "ge/net/socket.hpp"
#include "ge/proto/codec.hpp"

namespace ge::client {

struct PlayerView {
    std::uint32_t id;
    float x, y, z, yaw, pitch;
    std::uint16_t health, score, flags;
    std::uint8_t  fuel, ammo;
};

// A powerup as the client knows it. `active` is carried rather than the record
// being omitted, because the server sends inactive pickups too -- see the note
// on proto::PickupRecord. A client that only ever heard about active ones
// would keep drawing a pickup whose removal it missed.
struct PickupView {
    float        x, y, z;
    std::uint8_t kind;      // proto::PickupKind
    bool         active;
};

struct WorldView {
    std::uint64_t last_pong_ns = 0;   // transport RTT: never enters the simulation
    std::vector<PlayerView> players;
    std::vector<PickupView> pickups;
    std::uint32_t self_id   = 0;
    bool          have_self = false;
    float         world_size = 256.0f;
    std::uint32_t tick      = 0;
    std::uint64_t last_rtt_ns = 0;
    // The raw echoed input timestamp, not the derived RTT. A load generator
    // needs it to decide WHICH of its outstanding inputs this snapshot just
    // answered -- last_rtt_ns alone cannot distinguish "the input I sent 8 ms
    // ago" from "the input I sent 80 ms ago whose earlier snapshots were all
    // lost", and that difference is the whole measurement.
    std::uint64_t last_echo_ts_ns = 0;
    // Out-of-order discards, and gaps the tick numbers say never arrived.
    // Neither was counted before, so 30% loss looked like a clean link.
    std::uint64_t stale_snapshots  = 0;
    std::uint64_t missed_snapshots = 0;
};

class NetClient {
public:
    // UDP mode exists so the arena can be served by ge_arena_udp, whose
    // transport AF_XDP or DPDK could replace. The protocol is identical; only
    // the framing assumption changes -- over UDP each datagram is exactly one
    // frame, so there is no partial-frame state to carry between reads.
    bool connect_udp(const std::string& host, std::uint16_t port, std::string& err) {
        udp_ = true;
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { err = "socket: "; err += std::strerror(errno); return false; }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) {
            ::close(fd); err = "bad address " + host; return false;
        }
        // connect() on a datagram socket just fixes the peer, so send/recv work
        // without carrying an address and the kernel filters other sources.
        if (::connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0) {
            ::close(fd); err = "connect: "; err += std::strerror(errno); return false;
        }
        fd_ = net::Fd(fd);
        net::set_nonblocking(fd_.get());

        if (!send_hello()) { err = "send hello failed"; return false; }
        return true;
    }

    bool connect(const std::string& host, std::uint16_t port, std::string& err) {
        auto r = net::connect_tcp(host.c_str(), port, /*nonblocking=*/false);
        if (!r) { err = r.error().message; return false; }
        fd_ = std::move(*r);
        net::set_nodelay(fd_.get());
        net::set_nosigpipe(fd_.get());
        net::set_nonblocking(fd_.get());

        // The handshake is the one place the client blocks: there is nothing to
        // render until the server has told us the world size.
        alignas(8) std::byte buf[64];
        const proto::ClientHello hello{0xC0FFEEULL, proto::kProtocolVersion, 0};
        const std::size_t n = proto::encode(buf, hello, 0, 0);
        if (!send_all(buf, n)) { err = "send hello failed"; return false; }
        return true;
    }

    // Ping is answered on the I/O thread and never reaches a shard, so its RTT
    // is the transport cost alone -- no tick quantisation, no simulation.
    bool send_ping() {
        alignas(8) std::byte buf[sizeof(proto::FrameHeader) + sizeof(proto::Ping)];
        const proto::Ping p{std::uint64_t(core::Clock::now_ns()), 0};
        const std::size_t n = proto::encode(buf, p, session_, ++seq_);
        return send_all(buf, n);
    }

    // The timestamp stamped into the most recent input. It is the identity of
    // that input on the wire: the server echoes it back in the header of the
    // snapshot that first reflects it.
    std::uint64_t last_input_ts() const noexcept { return last_input_ts_; }

    bool send_input(float move_x, float move_z, float yaw, float pitch,
                    std::uint32_t buttons) {
        alignas(8) std::byte buf[sizeof(proto::FrameHeader) + sizeof(proto::ShooterInput)];
        proto::ShooterInput in{};
        in.client_ts_ns = std::uint64_t(core::Clock::now_ns());
        last_input_ts_  = in.client_ts_ns;
        // Its own counter. Sharing `seq_` with pings numbered consecutive
        // inputs 2 apart, which the server's gap counter read as 49.9% loss on
        // a lossless loopback. The counter was right; the sequence was not.
        in.input_seq    = ++input_seq_;
        in.move_x = move_x; in.move_z = move_z;
        in.yaw = yaw; in.pitch = pitch; in.buttons = buttons;
        const std::size_t n = proto::encode(buf, in, session_, ++seq_);
        // Uplink loss injection. The sequence is consumed either way: built,
        // numbered, dropped -- which is what the gap counter has to see.
        if (send_drop_pct_ > 0) {
            rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
            if (int((rng_ >> 33) % 100) < send_drop_pct_) return true;
        }
        return send_all(buf, n);
    }

    // Drains everything readable and folds it into `view`. Returns false if the
    // connection is gone.
    bool poll(WorldView& view) {
        retry_hello_if_unanswered();
        if (udp_) {
            // One datagram, one frame. A short or malformed datagram is dropped
            // rather than closing anything: over UDP there is nothing to close,
            // and a single bad packet must not end the session.
            for (;;) {
                alignas(8) std::byte dg[2048];
                const ssize_t n = ::recv(fd_.get(), dg, sizeof dg, MSG_DONTWAIT);
                if (n <= 0) break;
                if (drop_pct_ > 0) {
                    rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
                    if (int((rng_ >> 33) % 100) < drop_pct_) continue;   // "lost"
                }
                proto::FrameReader r({dg, std::size_t(n)});
                proto::FrameView f;
                if (r.next(f) == proto::ParseError::Ok) on_frame(f, view);
            }
            return true;
        }
        for (;;) {
            if (rx_.size() - used_ < 16384) rx_.resize(rx_.size() * 2);
            const net::IoOutcome in = net::read_some(fd_.get(), rx_.data() + used_,
                                                     rx_.size() - used_);
            if (in.result == net::IoResult::WouldBlock) break;
            if (in.result != net::IoResult::Ok) return false;   // Closed or Error
            used_ += std::size_t(in.bytes);
        }

        // One reader over everything buffered. A partial frame at the tail is
        // Incomplete, not an error: it is simply the rest of a TCP segment that
        // has not arrived, and it stays in the buffer for the next poll.
        proto::FrameReader reader({rx_.data(), used_});
        proto::FrameView f;
        for (;;) {
            const proto::ParseError e = reader.next(f);
            if (e == proto::ParseError::Incomplete) break;
            if (e != proto::ParseError::Ok) return false;       // desync: drop the link
            on_frame(f, view);
        }
        const std::size_t consumed = reader.consumed();
        if (consumed) {
            std::memmove(rx_.data(), rx_.data() + consumed, used_ - consumed);
            used_ -= consumed;
        }
        return true;
    }

    // Deliberate loss injection, for testing that the protocol's claimed
    // tolerance is real rather than asserted.
    void set_drop_pct(int pct) noexcept { drop_pct_ = pct; }
    // Discard a fraction of outbound inputs. Used to prove the server's
    // sequence-gap counter reports a loss rate it was not told, rather than
    // trusting a counter because it reads zero when nothing is wrong.
    void set_send_drop_pct(int pct) noexcept { send_drop_pct_ = pct; }
    // Seeded per client: identical seeds make N bots drop the same datagrams,
    // so the aggregate is one draw repeated, not N draws.
    void set_loss_seed(std::uint64_t s) noexcept {
        rng_ = s * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
    }

    std::uint32_t session() const noexcept { return session_; }
    bool ready() const noexcept { return session_ != 0; }
    // Retried to exhaustion with no welcome: the caller can say unreachable
    // instead of sending inputs nobody is listening to.
    bool handshake_failed() const noexcept {
        return udp_ && session_ == 0 && hello_attempts_ >= kMaxHelloAttempts;
    }

private:
    void on_frame(const proto::FrameView& f, WorldView& view) {
        switch (f.type()) {
            case proto::MsgType::ServerWelcome: {
                if (const auto* w = f.as<proto::ServerWelcome>()) {
                    session_        = w->session_id;
                    view.world_size = w->world_size;
                }
                break;
            }
            case proto::MsgType::Pong: {
                if (const auto* p = f.as<proto::Pong>())
                    view.last_pong_ns = std::uint64_t(core::Clock::now_ns()) - p->client_ts_ns;
                break;
            }
            case proto::MsgType::ShooterDelta: {
                // Two arrays in one tail, so the offsets come from the shared
                // reader rather than being recomputed here. tail<T>() no
                // longer compiles for this message, which is the point: there
                // is no single stride to span.
                const proto::ShooterDeltaView d(f);
                if (!d.valid()) break;

                // Datagrams reorder, and applying an older snapshot rewinds
                // everyone for a frame. Signed difference, not `<`: a plain
                // compare would discard everything once the uint32 wrapped.
                if (seen_tick_ && std::int32_t(d.header().tick - view.tick) < 0) {
                    ++view.stale_snapshots;
                    break;
                }
                // A gap means snapshots between the last one and this one were
                // lost. Counting them is what makes loss measurable from the
                // client rather than inferred from a delivery ratio.
                if (seen_tick_) {
                    const std::int32_t advance =
                        std::int32_t(d.header().tick - view.tick);
                    if (advance > 1)
                        view.missed_snapshots += std::uint64_t(advance - 1);
                }
                seen_tick_ = true;

                view.players.clear();
                view.players.reserve(d.players().size());
                view.have_self = false;
                for (const auto& r : d.players()) {
                    view.players.push_back(PlayerView{r.id, r.x, r.y, r.z, r.yaw,
                                                      r.pitch, r.health, r.score, r.flags,
                                                      r.fuel, r.ammo});
                    if (r.flags & proto::kPlayerSelf) {
                        view.self_id   = r.id;
                        view.have_self = true;
                    }
                }
                view.pickups.clear();
                view.pickups.reserve(d.pickups().size());
                for (const auto& k : d.pickups())
                    view.pickups.push_back(PickupView{k.x, k.y, k.z, k.kind,
                                                      (k.flags & proto::kPickupActive) != 0});
                view.tick = d.header().tick;
                if (d.header().echo_client_ts_ns) {
                    view.last_echo_ts_ns = d.header().echo_client_ts_ns;
                    view.last_rtt_ns = std::uint64_t(core::Clock::now_ns())
                                     - d.header().echo_client_ts_ns;
                }
                break;
            }
            default: break;
        }
    }

    // Sent once, over UDP, with no ack: lose it in a join burst and the client
    // never gets a session but keeps sending inputs the server discards.
    // Observed 144 of 512 joined while the server looked healthy. TCP
    // retransmits its own handshake, so this is datagram-only.
    void retry_hello_if_unanswered() noexcept {
        if (!udp_ || session_ != 0 || hello_attempts_ >= kMaxHelloAttempts) return;
        const std::uint64_t now = std::uint64_t(core::Clock::now_ns());
        if (now < hello_next_ns_) return;
        send_hello();
    }

    bool send_hello() noexcept {
        alignas(8) std::byte buf[64];
        const proto::ClientHello hello{0xC0FFEEULL, proto::kProtocolVersion, 0};
        const std::size_t n = proto::encode(buf, hello, 0, 0);
        ++hello_attempts_;
        hello_next_ns_ = std::uint64_t(core::Clock::now_ns()) + kHelloRetryNs;
        return send_all(buf, n);
    }

    // Blocking-until-sent on a non-blocking socket. Correct here because a
    // client sends one 48-byte frame per tick: the socket buffer is never
    // anywhere near full, so the spin is theoretical rather than real.
    bool send_all(const std::byte* p, std::size_t n) {
        if (udp_) {
            // A datagram is all-or-nothing; a partial send cannot happen.
            const ssize_t w = ::send(fd_.get(), p, n, 0);
            if (w >= 0) return true;
            return errno == EAGAIN || errno == EWOULDBLOCK;   // drop, don't fail
        }
        std::size_t off = 0;
        while (off < n) {
            const net::IoOutcome w = net::write_some(fd_.get(), p + off, n - off);
            if (w.result == net::IoResult::Ok)        { off += std::size_t(w.bytes); continue; }
            if (w.result == net::IoResult::WouldBlock) continue;
            return false;
        }
        return true;
    }

    net::Fd                fd_;
    // std::byte, not char: FrameReader asserts 8-byte alignment of its buffer,
    // and operator new[] gives at least max_align_t.
    std::vector<std::byte> rx_ = std::vector<std::byte>(65536);
    std::size_t            used_    = 0;
    bool                   udp_     = false;
    int                    drop_pct_ = 0;
    int                    send_drop_pct_ = 0;
    std::uint64_t          rng_      = 0x9E3779B97F4A7C15ULL;
    std::uint32_t     session_ = 0;
    std::uint32_t     seq_     = 0;     // frame sequence, every message type
    std::uint32_t     input_seq_ = 0;   // dense per-input sequence
    std::uint64_t     last_input_ts_ = 0;

    // Two seconds of patience, bounded so a client cannot hammer a dead host.
    static constexpr std::uint64_t kHelloRetryNs     = 100'000'000ULL;
    static constexpr int           kMaxHelloAttempts = 10;
    std::uint64_t     hello_next_ns_  = 0;
    int               hello_attempts_ = 0;
    bool              seen_tick_      = false;
};

} // namespace ge::client
