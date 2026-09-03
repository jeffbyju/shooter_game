// ge_arena_udp -- the arena on one core, over UDP.
//
// Every message is independently meaningful: inputs carry absolute angles and
// buttons, snapshots are full state rather than diffs. So one message fits one
// datagram, loss costs a single stale tick, and there is no stream to
// reassemble -- which is what would make recvmmsg/sendmmsg swappable for an
// AF_XDP or DPDK ring without touching anything above the transport.
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "ge/core/clock.hpp"
#include "ge/core/thread.hpp"
#include "ge/proto/codec.hpp"
#include "ge/world/arena.hpp"
#include "ge/world/grid.hpp"

namespace {

using ge::core::Clock;
namespace proto = ge::proto;
namespace world = ge::world;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

inline constexpr std::uint32_t kNoSlot = 0xffffffffu;

// --------------------------------------------------------------- batched I/O
//
// 2048 players at 60 Hz is ~370k syscalls/s one datagram at a time, and the
// process profiles at ~83% kernel. recvmmsg/sendmmsg carry up to kBatch per
// call instead.
//
// Linux only; elsewhere the batch collapses to 1 and the plain path runs. That
// is also how the A/B works -- same binary, --batch picks the path, so a
// difference cannot be a build difference.
inline constexpr int kMaxBatch   = 128;
inline constexpr int kRxSlotSize = 2048;   // >> the largest client frame (48 B)

#if defined(__linux__)
#define GE_HAVE_MMSG 1
#else
#define GE_HAVE_MMSG 0
#endif

// Receives up to `batch` datagrams per syscall.
class RxBatch {
public:
    void configure(int batch) {
        batch_ = std::clamp(batch, 1, kMaxBatch);
        buf_.assign(std::size_t(batch_) * kRxSlotSize, std::byte{});
#if GE_HAVE_MMSG
        for (int i = 0; i < batch_; ++i) {
            iov_[std::size_t(i)].iov_base = slot(i);
            iov_[std::size_t(i)].iov_len  = kRxSlotSize;
            auto& h = msgs_[std::size_t(i)].msg_hdr;
            h = {};
            h.msg_name    = &addrs_[std::size_t(i)];
            h.msg_namelen = sizeof(sockaddr_in);
            h.msg_iov     = &iov_[std::size_t(i)];
            h.msg_iovlen  = 1;
        }
#endif
    }
    int batch() const noexcept { return batch_; }

    // Returns the number of datagrams read, or <= 0 when the socket is drained.
    int recv(int fd) noexcept {
#if GE_HAVE_MMSG
        if (batch_ > 1) {
            // msg_namelen is an in/out parameter: the kernel overwrites it with
            // the address length it actually wrote, so it has to be reset every
            // call or a later batch silently truncates the source address.
            for (int i = 0; i < batch_; ++i)
                msgs_[std::size_t(i)].msg_hdr.msg_namelen = sizeof(sockaddr_in);
            return ::recvmmsg(fd, msgs_.data(), unsigned(batch_), MSG_DONTWAIT, nullptr);
        }
#endif
        socklen_t flen = sizeof(sockaddr_in);
        const ssize_t n = ::recvfrom(fd, slot(0), kRxSlotSize, MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr*>(&addrs_[0]), &flen);
        if (n <= 0) return int(n);
        len0_ = std::uint32_t(n);
        return 1;
    }

    const std::byte*   data(int i) const noexcept { return slot(i); }
    std::size_t        size(int i) const noexcept {
#if GE_HAVE_MMSG
        if (batch_ > 1) return msgs_[std::size_t(i)].msg_len;
#endif
        (void)i; return len0_;
    }
    const sockaddr_in& addr(int i) const noexcept { return addrs_[std::size_t(i)]; }

private:
    std::byte* slot(int i) noexcept { return buf_.data() + std::size_t(i) * kRxSlotSize; }
    const std::byte* slot(int i) const noexcept {
        return buf_.data() + std::size_t(i) * kRxSlotSize;
    }
    int                                     batch_ = 1;
    std::uint32_t                           len0_  = 0;
    std::vector<std::byte>                  buf_;
    std::array<sockaddr_in, kMaxBatch>      addrs_{};
#if GE_HAVE_MMSG
    std::array<mmsghdr, kMaxBatch>          msgs_{};
    std::array<iovec,   kMaxBatch>          iov_{};
#endif
};

// Accumulates outbound datagrams and flushes them with one syscall.
//
// The encoder writes straight into the staging slot rather than into a scratch
// buffer that is then copied, so batching costs no extra memcpy -- it only
// changes when the kernel is told about the bytes.
class TxBatch {
public:
    void configure(int batch) {
        batch_ = std::clamp(batch, 1, kMaxBatch);
        buf_.assign(std::size_t(batch_) * proto::kMaxFrameBytes, std::byte{});
    }
    int  batch() const noexcept { return batch_; }
    bool full()  const noexcept { return n_ >= batch_; }

    // Where to encode the next datagram. Always 8-byte aligned: the slot stride
    // is kMaxFrameBytes, which the protocol already requires be a multiple of 8.
    std::byte* staging() noexcept { return buf_.data() + std::size_t(n_) * proto::kMaxFrameBytes; }

    void commit(std::size_t len, const sockaddr_in& to) noexcept {
        lens_[std::size_t(n_)]  = len;
        addrs_[std::size_t(n_)] = to;
        ++n_;
    }

    // Returns {sent, failed}. A short sendmmsg is not an error to retry here:
    // the socket buffer is full, which is backpressure, and the next tick
    // carries strictly newer state. Shedding is the same policy the rest of
    // the server uses.
    struct Result { std::uint64_t sent = 0, failed = 0; };
    Result flush(int fd) noexcept {
        Result r{};
        if (n_ == 0) return r;
#if GE_HAVE_MMSG
        if (batch_ > 1) {
            for (int i = 0; i < n_; ++i) {
                iov_[std::size_t(i)].iov_base = buf_.data() + std::size_t(i) * proto::kMaxFrameBytes;
                iov_[std::size_t(i)].iov_len  = lens_[std::size_t(i)];
                auto& h = msgs_[std::size_t(i)].msg_hdr;
                h = {};
                h.msg_name    = &addrs_[std::size_t(i)];
                h.msg_namelen = sizeof(sockaddr_in);
                h.msg_iov     = &iov_[std::size_t(i)];
                h.msg_iovlen  = 1;
            }
            const int sent = ::sendmmsg(fd, msgs_.data(), unsigned(n_), 0);
            if (sent < 0) r.failed = std::uint64_t(n_);
            else { r.sent = std::uint64_t(sent); r.failed = std::uint64_t(n_ - sent); }
            n_ = 0;
            return r;
        }
#endif
        for (int i = 0; i < n_; ++i) {
            const ssize_t w = ::sendto(fd, buf_.data() + std::size_t(i) * proto::kMaxFrameBytes,
                                       lens_[std::size_t(i)], 0,
                                       reinterpret_cast<sockaddr*>(&addrs_[std::size_t(i)]),
                                       sizeof(sockaddr_in));
            if (w < 0) ++r.failed; else ++r.sent;
        }
        n_ = 0;
        return r;
    }

private:
    int                                    batch_ = 1;
    int                                    n_     = 0;
    std::vector<std::byte>                 buf_;
    std::array<std::size_t,  kMaxBatch>    lens_{};
    std::array<sockaddr_in,  kMaxBatch>    addrs_{};
#if GE_HAVE_MMSG
    std::array<mmsghdr, kMaxBatch>         msgs_{};
    std::array<iovec,   kMaxBatch>         iov_{};
#endif
};


struct Peer {
    sockaddr_in   addr{};
    std::uint32_t session    = 0;
    std::int64_t  last_seen  = 0;
    // The arena slot this peer occupies, kept current across the swap-with-last
    // that ArenaStore::remove performs. Without it, finding a peer's entity
    // means scanning every entity on every input packet -- which profiled at
    // 6% of the whole process at 2048 players.
    std::uint32_t arena_slot = kNoSlot;
    // Last input sequence seen from this peer. The protocol has carried `seq`
    // from the start and nothing ever compared two of them, so the server could
    // not tell a client losing 30% of its datagrams from one on a clean link.
    // For a project whose argument is loss tolerance, not measuring loss was
    // the gap worth closing.
    std::uint32_t last_seq   = 0;
    bool          has_seq    = false;
    bool          used       = false;
};

// (address, port) -> peer slot, open addressed with linear probing. Replaces a
// scan that ran once per inbound datagram -- ~220M comparisons/s at 2048
// players, and 21% of the server's CPU.
//
// Erase rebuilds the whole table rather than using tombstones. Only correct
// because eviction happens at most once a tick, and cheaper than a tombstone
// branch in a lookup that runs 200k times a second.
class PeerTable {
public:
    void init(std::size_t peers) {
        cap_ = 16;
        while (cap_ < peers * 2) cap_ <<= 1;
        keys_.assign(cap_, 0);
        vals_.assign(cap_, kNoSlot);
        mask_ = cap_ - 1;
    }

    static std::uint64_t key_of(const sockaddr_in& a) noexcept {
        return (std::uint64_t(a.sin_addr.s_addr) << 16) | std::uint64_t(a.sin_port);
    }

    std::uint32_t find(const sockaddr_in& a) const noexcept {
        const std::uint64_t k = key_of(a);
        for (std::size_t i = hash(k) & mask_; keys_[i] != 0; i = (i + 1) & mask_)
            if (keys_[i] == k) return vals_[i];
        return kNoSlot;
    }

    void insert(const sockaddr_in& a, std::uint32_t slot) noexcept {
        const std::uint64_t k = key_of(a);
        std::size_t i = hash(k) & mask_;
        while (keys_[i] != 0 && keys_[i] != k) i = (i + 1) & mask_;
        keys_[i] = k;
        vals_[i] = slot;
    }

    // Called after one or more peers have been marked unused.
    void rebuild(const std::vector<Peer>& peers) noexcept {
        std::fill(keys_.begin(), keys_.end(), std::uint64_t{0});
        std::fill(vals_.begin(), vals_.end(), kNoSlot);
        for (std::uint32_t s = 0; s < peers.size(); ++s)
            if (peers[s].used) insert(peers[s].addr, s);
    }

private:
    // splitmix64 finaliser: the key is an address and a port, and ports from one
    // host differ only in the low bits, so the low bits have to be spread or
    // every client behind one NAT lands in the same probe run.
    static std::uint64_t hash(std::uint64_t x) noexcept {
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27; x *= 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    std::vector<std::uint64_t> keys_;
    std::vector<std::uint32_t> vals_;
    std::size_t cap_ = 0, mask_ = 0;
};

struct Stats {
    std::uint64_t datagrams_in = 0, datagrams_out = 0, bad = 0;
    std::uint64_t joins = 0, inputs = 0, pings = 0, send_fail = 0;
    // Welcomes re-sent because a hello arrived from a peer we already know,
    // which means the first welcome was lost. A non-zero value here is the
    // only direct evidence that the join path is losing datagrams.
    std::uint64_t rewelcomes = 0;
    // Datagrams discarded because they came from an address with no session.
    // Previously these vanished: they were counted in `datagrams_in`, dropped,
    // and never attributed, so a client stuck outside the handshake looked
    // exactly like healthy traffic.
    std::uint64_t no_session = 0;
    // Inputs the sequence numbers say were sent and never arrived, summed over
    // all peers. The denominator is `inputs`.
    std::uint64_t input_gaps = 0;
    // Syscalls actually issued, against datagrams carried. datagrams/syscall is
    // the batching efficiency number and the first thing to look at when
    // throughput stops scaling.
    std::uint64_t rx_syscalls = 0, rx_batched = 0;
    std::uint64_t tx_syscalls = 0, tx_batched = 0;
    std::uint64_t aoi_candidates = 0;
};

void usage(const char* a0) {
    std::printf(
"usage: %s [options]\n"
"  --host H      bind address        (default 0.0.0.0)\n"
"  --port P      bind port           (default 9000)\n"
"  --tick-hz N   simulation rate     (default 60)\n"
"  --world F     arena size          (default 200)\n"
"  --aoi F       area of interest    (default 400)\n"
"  --max N       max players         (default 256)\n"
"  --pickups N   powerups on the map (default 12, max %zu)\n"
"  --aoi-scan    skip the spatial grid and scan every entity, as the\n"
"                original did -- the A/B for the AOI work, same binary\n"
"  --batch N     datagrams per syscall; 1 = one recvfrom/sendto each\n"
"                (default 64 on Linux, 1 elsewhere -- recvmmsg/sendmmsg\n"
"                 do not exist on macOS)\n"
"  --quiet       no periodic stats\n", a0, proto::kMaxPickups);
}

} // namespace

int main(int argc, char** argv) {
    const char* host = "0.0.0.0";
    std::uint16_t port = 9000;
    int tick_hz = 60, max_players = 256;
    int n_pickups = int(proto::kMaxPickups);
    float world_size = 200.0f, aoi = 400.0f;
    bool quiet = false;
    int  batch_size = GE_HAVE_MMSG ? 64 : 1;
    bool aoi_scan   = false;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
            return argv[++i];
        };
        if      (!std::strcmp(argv[i], "--host"))    host = next();
        else if (!std::strcmp(argv[i], "--port"))    port = std::uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--tick-hz")) tick_hz = std::atoi(next());
        else if (!std::strcmp(argv[i], "--world"))   world_size = float(std::atof(next()));
        else if (!std::strcmp(argv[i], "--aoi"))     aoi = float(std::atof(next()));
        else if (!std::strcmp(argv[i], "--max"))     max_players = std::atoi(next());
        else if (!std::strcmp(argv[i], "--pickups")) n_pickups = std::atoi(next());
        else if (!std::strcmp(argv[i], "--aoi-scan")) aoi_scan = true;
        else if (!std::strcmp(argv[i], "--batch")) batch_size = std::atoi(next());
        else if (!std::strcmp(argv[i], "--quiet"))   quiet = true;
        else { usage(argv[0]); return 2; }
    }

    // Clamped rather than rejected: kMaxPickups is what every downstream
    // buffer was sized for, so exceeding it would be a frame that does not fit
    // rather than a configuration the operator meant.
    if (n_pickups < 0) n_pickups = 0;
    if (n_pickups > int(proto::kMaxPickups)) n_pickups = int(proto::kMaxPickups);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int bufsz = 4 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);

    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host, &me.sin_addr) != 1) {
        std::fprintf(stderr, "bad --host %s\n", host); return 1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) {
        std::perror("bind"); return 1;
    }

    world::ArenaStore arena;
    arena.reserve(std::size_t(max_players));
    // Named, for the same most-vexing-parse reason as in apps/shooter_bots.
    const std::size_t peer_cap = std::size_t(max_players);
    std::vector<Peer> peers(peer_cap);
    PeerTable peer_table;
    peer_table.init(peer_cap);
    // Free slots as a stack rather than a scan for `!used`. The scan was O(max
    // players) per join, which is invisible one player at a time and 2 million
    // comparisons when 2048 clients connect at once.
    std::vector<std::uint32_t> free_slots;
    free_slots.reserve(peer_cap);
    for (std::size_t i = peer_cap; i-- > 0;) free_slots.push_back(std::uint32_t(i));
    ge::core::Xoshiro256ss rng{0xA7E9A};
    world::PickupField pickups;
    pickups.init(std::uint16_t(n_pickups), world_size, rng);

    // AOI index. The delta builder used to test every player against every
    // other -- 4.2M distance tests a tick at 2048 players, ~13% of the process.
    // Cell size = AOI radius, so a query touches nine cells at most.
    world::SpatialGrid aoi_grid;
    aoi_grid.configure(world::Bounds{0.0f, 0.0f, world_size, world_size},
                       aoi, std::size_t(max_players));
    const std::uint64_t pickup_respawn_ticks =
        std::uint64_t(world::kPickupRespawnSec) * std::uint64_t(tick_hz);

    Stats st;
    std::uint32_t next_session = 1, next_id = 1;
    const float dt = 1.0f / float(tick_hz);
    const std::int64_t period_ns = 1'000'000'000LL / tick_hz;
    std::int64_t next_tick = Clock::now_ns();
    std::uint64_t tick = 0;
    std::int64_t last_report = Clock::now_ns();
    std::uint64_t last_report_tick = 0;

    // Receive and send staging, sized once. Nothing below allocates.
    RxBatch rx; rx.configure(batch_size);
    TxBatch tx; tx.configure(batch_size);
    // Scratch for the small replies (welcome, pong) that are answered inline
    // rather than batched: they are rare next to snapshots, and keeping them
    // off the batch avoids interleaving a reply into the middle of a tick's
    // snapshot flush.
    alignas(8) std::byte sbuf[proto::kMaxFrameBytes];

    std::printf("ge_arena_udp on %s:%u -- %d Hz, world %.0f, aoi %.0f, max %d players\n",
                host, port, tick_hz, double(world_size), double(aoi), max_players);
    std::printf("  transport: UDP datagrams, one message per datagram\n");
    std::printf("  powerups:  %d on the map (half fuel, half ammo), %ds respawn\n",
                n_pickups, world::kPickupRespawnSec);

    auto find_peer = [&](const sockaddr_in& a) -> std::uint32_t {
        return peer_table.find(a);
    };

    while (!g_stop) {
        // ---- 1. drain the socket -------------------------------------------
        // Every datagram is exactly one frame. There is no reassembly, no
        // partial-frame state, and no per-peer receive buffer -- which is the
        // simplification that makes a bypass transport a drop-in here.
        for (;;) {
            const int got = rx.recv(fd);
            if (got <= 0) break;
            st.datagrams_in += std::uint64_t(got);
            st.rx_syscalls  += 1;
            st.rx_batched   += std::uint64_t(got);

          for (int bi = 0; bi < got; ++bi) {
            sockaddr_in     from = rx.addr(bi);
            const socklen_t flen = sizeof from;

            proto::FrameReader reader({rx.data(bi), rx.size(bi)});
            proto::FrameView f;
            if (reader.next(f) != proto::ParseError::Ok) { ++st.bad; continue; }

            const std::uint32_t pi = find_peer(from);
            const std::int64_t now = Clock::now_ns();

            switch (f.type()) {
                case proto::MsgType::ClientHello: {
                    // A hello from a known peer is a retry, not a duplicate:
                    // our welcome was lost. Dropping it left the client
                    // sessionless forever, still sending inputs we discarded.
                    // Join is idempotent, so just answer again.
                    if (pi != 0xffffffffu) {
                        const Peer& ex = peers[pi];
                        const proto::ServerWelcome rw{ge::core::mix64(ex.session),
                                                      ex.session, world_size, aoi, 0, 1,
                                                      std::uint16_t(tick_hz), 0, 0};
                        const std::size_t rlen =
                            proto::encode(sbuf, rw, ex.session, 0,
                                          std::uint8_t(proto::FrameFlags::ServerSent));
                        if (::sendto(fd, sbuf, rlen, 0,
                                     reinterpret_cast<sockaddr*>(&from), flen) < 0)
                            ++st.send_fail;
                        else { ++st.datagrams_out; ++st.rewelcomes; }
                        break;
                    }
                    if (arena.size() >= std::size_t(max_players)) break;
                    if (free_slots.empty()) break;
                    const std::uint32_t slot = free_slots.back();
                    free_slots.pop_back();

                    const std::uint32_t session = next_session++;
                    const float margin = 4.0f, span = world_size - 2.0f * margin;
                    const float sx = margin + float(rng.next_double()) * span;
                    const float sz = margin + float(rng.next_double()) * span;
                    const auto idx = arena.add(next_id++, sx, sz,
                                               world::ClientLink{session, slot, 0, 0});
                    peers[slot] = Peer{from, session, now, idx, 0, false, true};
                    peer_table.insert(from, slot);
                    ++st.joins;

                    const proto::ServerWelcome w{ge::core::mix64(session), session,
                                                 world_size, aoi, 0, 1,
                                                 std::uint16_t(tick_hz), 0, 0};
                    const std::size_t len = proto::encode(sbuf, w, session, 0,
                                        std::uint8_t(proto::FrameFlags::ServerSent));
                    if (::sendto(fd, sbuf, len, 0, reinterpret_cast<sockaddr*>(&from),
                                 flen) < 0) ++st.send_fail;
                    else ++st.datagrams_out;
                    break;
                }
                case proto::MsgType::Ping: {
                    // Answered inline, exactly as the TCP server answers it on
                    // the I/O thread: a ping must never enter the simulation or
                    // it measures tick quantisation instead of the transport.
                    const auto* p = f.as<proto::Ping>();
                    if (!p) break;
                    ++st.pings;
                    const proto::Pong pong{p->client_ts_ns, p->cookie,
                                           std::uint64_t(now)};
                    const std::size_t len = proto::encode(sbuf, pong,
                                        pi == 0xffffffffu ? 0 : peers[pi].session, 0,
                                        std::uint8_t(proto::FrameFlags::ServerSent));
                    if (::sendto(fd, sbuf, len, 0, reinterpret_cast<sockaddr*>(&from),
                                 flen) < 0) ++st.send_fail;
                    else ++st.datagrams_out;
                    break;
                }
                case proto::MsgType::ShooterInput: {
                    if (pi == 0xffffffffu) { ++st.no_session; break; }
                    const auto* in = f.as<proto::ShooterInput>();
                    if (!in) break;
                    ++st.inputs;
                    peers[pi].last_seen = now;

                    // Signed difference: a wrap must not read as two billion
                    // lost, and a negative advance is reordering, not loss.
                    if (peers[pi].has_seq) {
                        const std::int32_t adv =
                            std::int32_t(in->input_seq - peers[pi].last_seq);
                        if (adv > 1) st.input_gaps += std::uint64_t(adv - 1);
                    }
                    if (!peers[pi].has_seq ||
                        std::int32_t(in->input_seq - peers[pi].last_seq) > 0) {
                        peers[pi].last_seq = in->input_seq;
                        peers[pi].has_seq  = true;
                    }
                    // The slot is carried on the peer and fixed up when a
                    // removal moves an entity, so this is a direct index. The
                    // session is still checked, because a stale slot would
                    // otherwise apply one client's input to another's player.
                    const std::uint32_t k = peers[pi].arena_slot;
                    if (k < arena.size() && arena.link(k).session == peers[pi].session)
                        arena.set_input(k, in->move_x, in->move_z, in->yaw,
                                        in->pitch, in->buttons, in->input_seq,
                                        in->client_ts_ns);
                    break;
                }
                default: break;
            }
          }  // for bi: one datagram of the batch
          // A short read means the socket is drained; a full one means there
          // may be more waiting, so go round again.
          if (got < rx.batch()) break;
        }

        // ---- 2. wait for the next tick, or for a datagram --------------------
        // Block rather than spin. `if (now < next_tick) continue;` polls flat
        // out between ticks and pins a core -- on a 2-vCPU box that starved the
        // server and measured 64 ms of ping against a 36 ms link.
        std::int64_t now = Clock::now_ns();
        if (now < next_tick) {
            const std::int64_t left_ns = next_tick - now;
            pollfd pfd{fd, POLLIN, 0};
            ::poll(&pfd, 1, int(left_ns / 1'000'000));
            now = Clock::now_ns();
            if (now < next_tick) continue;   // woken by a datagram: go drain it
        }
        next_tick += period_ns;
        if (now - next_tick > period_ns * 4) next_tick = now + period_ns;  // fell behind: resync

        arena.clear_tick_flags();
        for (std::uint32_t i = 0; i < arena.size(); ++i)
            if (arena.due_respawn(i, tick)) {
                const float margin = 4.0f, span = world_size - 2.0f * margin;
                arena.spawn(i, margin + float(rng.next_double()) * span,
                               margin + float(rng.next_double()) * span);
            }
        arena.step(dt, world_size);
        for (std::uint32_t i = 0; i < arena.size(); ++i) {
            if (!arena.wants_fire(i)) continue;
            arena.consume_shot(i);
            const world::HitResult h = arena.hitscan(i);
            if (h.hit && arena.damage(h.victim, world::kHitDamage, tick, 180))
                arena.add_score(i);
        }
        // Powerups after movement, so walking onto one is collected on the
        // tick you reach it rather than the tick after.
        pickups.update(tick, world_size, rng);
        arena.collect_pickups(pickups, tick, pickup_respawn_ticks);

        if (!aoi_scan) aoi_grid.rebuild(arena.xs(), arena.zs(), arena.size());

        // ---- 3. one delta datagram per player -------------------------------
        const float r2 = aoi * aoi;
        for (std::uint32_t i = 0; i < arena.size(); ++i) {
            const std::uint32_t slot = arena.link(i).conn_id;
            if (slot >= peers.size() || !peers[slot].used) continue;

            proto::PlayerRecord recs[proto::kMaxAoiPlayers];
            std::uint16_t count = 0;
            auto emit = [&](std::uint32_t o) {
                proto::PlayerRecord& r = recs[count++];
                r = {};
                r.id = arena.id(o); r.x = arena.x(o); r.y = arena.y(o); r.z = arena.z(o);
                r.yaw = arena.yaw(o); r.pitch = arena.pitch(o);
                r.health = arena.health(o); r.score = arena.score(o);
                r.fuel = arena.fuel_pct(o); r.ammo = std::uint8_t(arena.ammo(o));
                r.flags = std::uint16_t(arena.tick_flags(o) |
                                        (o == i ? proto::kPlayerSelf : 0) |
                                        (arena.alive(o) ? 0 : proto::kPlayerDead));
            };
            emit(i);
            const float px = arena.x(i), pz = arena.z(i);
            if (aoi_scan) {
                // The original: every player against every other. Kept behind
                // a flag so the A/B is one binary and a difference cannot be
                // a build difference.
                for (std::uint32_t o = 0; o < arena.size() &&
                                          count < proto::kMaxAoiPlayers; ++o) {
                    if (o == i) continue;
                    const float dx = arena.x(o) - px, dz = arena.z(o) - pz;
                    ++st.aoi_candidates;
                    if (dx * dx + dz * dz > r2) continue;
                    emit(o);
                }
            } else {
                aoi_grid.query_aabb(px, pz, aoi, [&](std::span<const std::uint32_t> cell) {
                    st.aoi_candidates += cell.size();
                    for (const std::uint32_t o : cell) {
                        if (count >= proto::kMaxAoiPlayers) return;
                        if (o == i) continue;
                        const float dx = arena.x(o) - px, dz = arena.z(o) - pz;
                        if (dx * dx + dz * dz > r2) continue;
                        emit(o);
                    }
                });
            }

            // Every powerup, every tick, to every player -- not only the ones
            // in range and not only the ones that changed. 12 records is 192
            // bytes; the alternative is an event stream that needs
            // acknowledgement to survive a dropped datagram, which is the
            // complexity this protocol exists to avoid.
            proto::PickupRecord picks[proto::kMaxPickups];
            const std::uint16_t np = pickups.size();
            for (std::uint16_t k = 0; k < np; ++k) {
                proto::PickupRecord& r = picks[k];
                r = {};
                r.x = pickups.x(k); r.y = pickups.y(k); r.z = pickups.z(k);
                r.id = k;
                r.kind = pickups.kind(k);
                r.flags = std::uint8_t(pickups.active(k) ? proto::kPickupActive : 0);
            }

            proto::ShooterDeltaHeader hdr{};
            hdr.echo_client_ts_ns = arena.last_ts(i);
            hdr.tick              = std::uint32_t(tick);
            hdr.input_seq_ack     = arena.last_seq(i);
            hdr.count             = count;
            hdr.pickup_count      = np;

            // Straight into the batch slot -- batching adds no copy.
            const std::size_t len = proto::encode_flat2<proto::ShooterDeltaHeader,
                                                        proto::PlayerRecord,
                                                        proto::PickupRecord>(
                std::span<std::byte>(tx.staging(), proto::kMaxFrameBytes),
                hdr, std::span<const proto::PlayerRecord>(recs, count),
                std::span<const proto::PickupRecord>(picks, np),
                peers[slot].session, std::uint32_t(tick),
                std::uint8_t(proto::FrameFlags::ServerSent));
            if (len == 0) continue;
            tx.commit(len, peers[slot].addr);
            if (tx.full()) {
                const TxBatch::Result r = tx.flush(fd);
                st.datagrams_out += r.sent;
                st.send_fail     += r.failed;
                st.tx_syscalls   += 1;
                st.tx_batched    += r.sent;
            }
        }
        {   // whatever is left over at the end of the tick
            const TxBatch::Result r = tx.flush(fd);
            if (r.sent || r.failed) {
                st.datagrams_out += r.sent;
                st.send_fail     += r.failed;
                st.tx_syscalls   += 1;
                st.tx_batched    += r.sent;
            }
        }

        // ---- 4. evict silent peers -----------------------------------------
        // UDP has no close, so absence is the only disconnect signal there is.
        bool evicted = false;
        for (std::uint32_t s = 0; s < peers.size(); ++s) {
            if (!peers[s].used || now - peers[s].last_seen < 5'000'000'000LL) continue;
            const std::uint32_t k = peers[s].arena_slot;
            if (k < arena.size() && arena.link(k).session == peers[s].session) {
                arena.remove(k);
                // remove() swaps the last entity into `k`, so whoever owned the
                // last slot now owns this one. ClientLink::conn_id is the peer
                // slot, which is what makes the fix-up a single assignment
                // rather than another scan.
                if (k < arena.size()) {
                    const std::uint32_t moved = arena.link(k).conn_id;
                    if (moved < peers.size()) peers[moved].arena_slot = k;
                }
            }
            peers[s].used = false;
            peers[s].arena_slot = kNoSlot;
            free_slots.push_back(s);
            evicted = true;
        }
        if (evicted) peer_table.rebuild(peers);

        ++tick;
        if (!quiet && now - last_report > 1'000'000'000LL) {
            last_report = now;
            // Real ticks, not an assumed 60: when the server falls behind -- the
            // case this counter exists to show -- a fixed divisor understates it.
            const std::uint64_t ticks_since = tick - last_report_tick;
            last_report_tick = tick;
            std::printf("players %3zu  in %6llu/s  out %6llu/s  joins %llu  bad %llu  "
                        "sendfail %llu  nosession %llu  inputgaps %llu  rewelcome %llu  "
                        "dg/syscall rx %.1f tx %.1f  aoicand/tick %.0f\n",
                        arena.size(),
                        static_cast<unsigned long long>(st.datagrams_in),
                        static_cast<unsigned long long>(st.datagrams_out),
                        static_cast<unsigned long long>(st.joins),
                        static_cast<unsigned long long>(st.bad),
                        static_cast<unsigned long long>(st.send_fail),
                        static_cast<unsigned long long>(st.no_session),
                        static_cast<unsigned long long>(st.input_gaps),
                        static_cast<unsigned long long>(st.rewelcomes),
                        st.rx_syscalls ? double(st.rx_batched) / double(st.rx_syscalls) : 0.0,
                        st.tx_syscalls ? double(st.tx_batched)   / double(st.tx_syscalls) : 0.0,
                        ticks_since ? double(st.aoi_candidates) / double(ticks_since)
                                    : 0.0);
            st.datagrams_in = st.datagrams_out = 0;
            st.rx_syscalls = st.rx_batched = st.tx_syscalls = st.tx_batched = 0;
            st.aoi_candidates = 0;
        }
    }

    std::printf("\nstopped: %llu joins, %llu inputs, %llu pings, %llu bad frames\n",
                static_cast<unsigned long long>(st.joins),
                static_cast<unsigned long long>(st.inputs),
                static_cast<unsigned long long>(st.pings),
                static_cast<unsigned long long>(st.bad));
    std::printf("         %llu re-welcomes, %llu datagrams with no session, "
                "%llu inputs lost in transit (%.3f%%)\n",
                static_cast<unsigned long long>(st.rewelcomes),
                static_cast<unsigned long long>(st.no_session),
                static_cast<unsigned long long>(st.input_gaps),
                st.inputs ? 100.0 * double(st.input_gaps) /
                            double(st.inputs + st.input_gaps) : 0.0);
    ::close(fd);
    return 0;
}
