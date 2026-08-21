// ge_arena_udp -- the arena served over UDP datagrams.
//
// WHY THIS EXISTS
// ---------------
// AF_XDP and DPDK deliver raw Ethernet frames. Neither gives you TCP, so a
// server that needs a byte stream cannot use either without a userspace TCP
// stack (see docs/af-xdp/01-why-the-server-cannot-use-it.md). The way to make
// kernel bypass *possible* is therefore not to port the transport -- it is to
// stop needing a stream.
//
// The arena protocol was designed for that from the start:
//
//   * ShooterInput carries ABSOLUTE look angles and an absolute button bitset,
//     not deltas. A lost input costs one stale tick and nothing else; there is
//     no state to resynchronise.
//   * ShooterDelta is a FULL snapshot of everything visible, not a diff. A lost
//     delta is superseded by the next one 16.7 ms later.
//
// Both properties make every message independently meaningful, so each one fits
// in a single datagram and loss is survivable. That is the whole rewrite: the
// game logic is unchanged, the wire format is unchanged, and the transport
// becomes replaceable.
//
// WHAT BYPASS WOULD REPLACE
// -------------------------
// The two syscalls below, and nothing else:
//
//   recvmmsg()  ->  read descriptors from an AF_XDP RX ring (or DPDK rx_burst)
//   sendmmsg()  ->  write descriptors to an AF_XDP TX ring (or DPDK tx_burst)
//
// Everything between them -- parse, simulate, encode -- is untouched. The
// measured cost of that swap is in docs/af-xdp/05-results.md: ~1.56 us per
// traversal on Graviton3.
#include <algorithm>
#include <arpa/inet.h>
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

// (address, port) -> peer slot. Open addressed with linear probing over a
// power-of-two table, so a lookup is a hash and a handful of contiguous probes
// rather than a scan of every slot.
//
// The scan it replaces ran once per *inbound datagram*: at 2048 players sending
// 120 datagrams a second each it was ~220 million address comparisons a second,
// and it measured as 21% of the server's CPU -- the single largest cost in the
// process, larger than the simulation it exists to feed.
//
// Erase is a full rebuild rather than tombstones or backward-shift deletion.
// That is only correct because eviction happens at most once per tick and only
// for peers that have been silent for five seconds; rebuilding a 4096-entry
// table is far cheaper than carrying a tombstone branch through the lookup that
// runs 200000 times a second.
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

    // Area-of-interest index. The delta builder used to test every player
    // against every other player -- 2048 squared is 4.2 million distance tests
    // per tick, which profiled at about 13% of the process and is the term that
    // makes player count quadratic rather than linear.
    //
    // A cell the size of the AOI radius means a query touches at most nine
    // cells, and the uniform grid is rebuilt by counting sort in three linear
    // passes, so the per-tick cost goes from O(n^2) to O(n + n*k) where k is the
    // handful of players actually nearby.
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

    // One receive buffer reused every iteration. 2 KiB is well above the
    // largest message the client can send (a 48-byte ShooterInput frame).
    alignas(8) std::byte rbuf[2048];
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
            sockaddr_in from{};
            socklen_t flen = sizeof from;
            const ssize_t n = ::recvfrom(fd, rbuf, sizeof rbuf, MSG_DONTWAIT,
                                         reinterpret_cast<sockaddr*>(&from), &flen);
            if (n <= 0) break;
            ++st.datagrams_in;

            proto::FrameReader reader({rbuf, std::size_t(n)});
            proto::FrameView f;
            if (reader.next(f) != proto::ParseError::Ok) { ++st.bad; continue; }

            const std::uint32_t pi = find_peer(from);
            const std::int64_t now = Clock::now_ns();

            switch (f.type()) {
                case proto::MsgType::ClientHello: {
                    // A hello from a peer we already know is a RETRY, not a
                    // duplicate: it means our ServerWelcome was lost. Dropping
                    // it silently -- which this did -- leaves the client
                    // sessionless forever while it goes on sending inputs that
                    // land here, get counted in `in`, and are discarded. The
                    // join then fails in a way neither end can see.
                    //
                    // Re-answering is safe because join is idempotent: the
                    // session, slot and entity already exist, so this re-sends
                    // the same welcome rather than creating a second player.
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

                    // Signed difference so a uint32 wrap does not read as a
                    // two-billion-datagram loss. A negative advance is a
                    // reordered datagram, not a gap -- UDP may deliver out of
                    // order, and counting that as loss would overstate it.
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
        }

        // ---- 2. wait for the next tick, or for a datagram --------------------
        // Blocking here rather than spinning. The first version was
        // `if (now < next_tick) continue;`, which polls recvfrom flat out
        // between ticks: it pinned a whole core, and on a 2-vCPU box that
        // starved the server itself -- ping latency measured 64 ms when the
        // link RTT was 36 ms. poll() with the remaining time sleeps until
        // either a datagram lands or the tick is due, so the loop is idle
        // exactly when it has nothing to do.
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

        aoi_grid.rebuild(arena.xs(), arena.zs(), arena.size());

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
            aoi_grid.query_aabb(px, pz, aoi, [&](std::span<const std::uint32_t> cell) {
                for (const std::uint32_t o : cell) {
                    if (count >= proto::kMaxAoiPlayers) return;
                    if (o == i) continue;
                    const float dx = arena.x(o) - px, dz = arena.z(o) - pz;
                    if (dx * dx + dz * dz > r2) continue;
                    emit(o);
                }
            });

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

            const std::size_t len = proto::encode_flat2<proto::ShooterDeltaHeader,
                                                        proto::PlayerRecord,
                                                        proto::PickupRecord>(
                sbuf, hdr, std::span<const proto::PlayerRecord>(recs, count),
                std::span<const proto::PickupRecord>(picks, np),
                peers[slot].session, std::uint32_t(tick),
                std::uint8_t(proto::FrameFlags::ServerSent));
            if (len == 0) continue;
            if (::sendto(fd, sbuf, len, 0,
                         reinterpret_cast<sockaddr*>(&peers[slot].addr),
                         sizeof(sockaddr_in)) < 0) ++st.send_fail;
            else ++st.datagrams_out;
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
            std::printf("players %3zu  in %6llu/s  out %6llu/s  joins %llu  bad %llu  "
                        "sendfail %llu  nosession %llu  inputgaps %llu  rewelcome %llu\n",
                        arena.size(),
                        static_cast<unsigned long long>(st.datagrams_in),
                        static_cast<unsigned long long>(st.datagrams_out),
                        static_cast<unsigned long long>(st.joins),
                        static_cast<unsigned long long>(st.bad),
                        static_cast<unsigned long long>(st.send_fail),
                        static_cast<unsigned long long>(st.no_session),
                        static_cast<unsigned long long>(st.input_gaps),
                        static_cast<unsigned long long>(st.rewelcomes));
            st.datagrams_in = st.datagrams_out = 0;
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
