#include "test.hpp"

#include <cstring>
#include <vector>

#include "ge/core/bits.hpp"
#include "ge/proto/codec.hpp"

using namespace ge;
using namespace ge::proto;

namespace {
alignas(8) std::byte g_buf[4096];
}

GE_TEST(proto_encode_decode_roundtrip) {
    const Ping p{0x1122334455667788ULL, 0xdeadbeefULL};
    const std::size_t n = encode(g_buf, p, 42, 7);
    CHECK_EQ(n, sizeof(FrameHeader) + sizeof(Ping));

    FrameReader r(std::span<const std::byte>(g_buf, n));
    FrameView f;
    CHECK(r.next(f) == ParseError::Ok);
    CHECK(f.type() == MsgType::Ping);
    CHECK_EQ(f.session(), 42u);
    CHECK_EQ(f.header().seq, 7u);

    const Ping* back = f.as<Ping>();
    CHECK(back != nullptr);
    CHECK_EQ(back->client_ts_ns, p.client_ts_ns);
    CHECK_EQ(back->cookie, p.cookie);
}

// The zero-copy claim, stated as a test: the decoded pointer must be *inside*
// the receive buffer, not a copy of it.
GE_TEST(proto_decode_aliases_the_receive_buffer) {
    const MoveCommand m{999, 3, 1.5f, -2.5f, 0};
    const std::size_t n = encode(g_buf, m, 1, 1);
    FrameReader r(std::span<const std::byte>(g_buf, n));
    FrameView f;
    CHECK(r.next(f) == ParseError::Ok);

    const MoveCommand* p = f.as<MoveCommand>();
    CHECK(p != nullptr);
    const auto* raw = reinterpret_cast<const std::byte*>(p);
    CHECK(raw >= g_buf && raw < g_buf + n);
    CHECK_EQ(raw, g_buf + sizeof(FrameHeader));

    // Mutating the buffer is visible through the view: proof there is no copy.
    auto* mut = reinterpret_cast<MoveCommand*>(g_buf + sizeof(FrameHeader));
    mut->input_seq = 12345;
    CHECK_EQ(p->input_seq, 12345u);
}
// A SnakeDelta's tail is a run of (SnakeRecord, Point2[segments]) pairs, so the
// stride is not constant and the reader has to trust nothing.
static std::size_t build_delta(std::byte* buf, std::size_t cap,
                               std::uint16_t n, std::uint16_t segs) {
    SnakeDeltaWriter w(std::span<std::byte>(buf, cap), 7, 100);
    for (std::uint16_t s = 0; s < n; ++s) {
        auto pts = w.begin_snake(segs);
        if (pts.empty() && segs > 0) break;
        for (std::uint16_t g = 0; g < segs; ++g)
            pts[g] = Point2{float(s * 100 + g), float(g)};
        SnakeRecord rec{};
        rec.id     = 1000u + s;
        rec.flags  = std::uint16_t(s == 0 ? kSnakeSelf : 0);
        rec.length = segs;
        rec.kills  = s;
        w.commit_snake(rec, segs);
    }
    SnakeDeltaHeader hdr{};
    hdr.echo_client_ts_ns = 0xABCD;
    hdr.tick = 100;
    return w.finish(hdr);
}

GE_TEST(proto_snake_delta_roundtrip_and_aliases_buffer) {
    const std::size_t n = build_delta(g_buf, sizeof g_buf, 3, 5);

    FrameReader r(std::span<const std::byte>(g_buf, n));
    FrameView f;
    CHECK(r.next(f) == ParseError::Ok);
    CHECK(f.type() == MsgType::SnakeDelta);

    SnakeDeltaView v(f);
    CHECK(v.valid());
    CHECK_EQ(v.header().count, 3u);
    CHECK_EQ(v.header().echo_client_ts_ns, 0xABCDu);

    const SnakeRecord* rec = nullptr;
    std::span<const Point2> pts;
    int seen = 0;
    while (v.next(rec, pts)) {
        CHECK_EQ(pts.size(), 5u);
        CHECK_EQ(rec->id, std::uint32_t(1000 + seen));
        CHECK_EQ(pts[0].x, float(seen * 100));
        // The points lie inside the frame we parsed: zero copy, not a copy.
        const auto* raw = reinterpret_cast<const std::byte*>(pts.data());
        CHECK(raw > g_buf && raw < g_buf + n);
        ++seen;
    }
    CHECK_EQ(seen, 3);
}

GE_TEST(proto_snake_delta_marks_exactly_one_self) {
    const std::size_t n = build_delta(g_buf, sizeof g_buf, 4, 3);
    FrameReader r(std::span<const std::byte>(g_buf, n));
    FrameView f;
    r.next(f);
    SnakeDeltaView v(f);
    const SnakeRecord* rec = nullptr;
    std::span<const Point2> pts;
    int selves = 0;
    while (v.next(rec, pts)) if (rec->flags & kSnakeSelf) ++selves;
    CHECK_EQ(selves, 1);
}

// Snakes differ in length, so the reader must advance by each record's own
// segment count rather than a fixed stride.
GE_TEST(proto_snake_delta_handles_variable_lengths) {
    SnakeDeltaWriter w(g_buf, 1, 1);
    const std::uint16_t lens[] = {1, 9, 2, 48, 5};
    for (std::uint16_t s = 0; s < 5; ++s) {
        auto pts = w.begin_snake(lens[s]);
        CHECK(!pts.empty());
        for (std::uint16_t g = 0; g < lens[s]; ++g) pts[g] = Point2{float(s), float(g)};
        SnakeRecord rec{};
        rec.id = s;
        w.commit_snake(rec, lens[s]);
    }
    const std::size_t n = w.finish(SnakeDeltaHeader{});

    FrameReader r(std::span<const std::byte>(g_buf, n));
    FrameView f;
    r.next(f);
    SnakeDeltaView v(f);
    const SnakeRecord* rec = nullptr;
    std::span<const Point2> pts;
    std::uint16_t i = 0;
    while (v.next(rec, pts)) {
        CHECK_EQ(pts.size(), std::size_t(lens[i]));
        CHECK_EQ(rec->id, std::uint32_t(i));
        CHECK_EQ(pts[0].x, float(i));
        ++i;
    }
    CHECK_EQ(i, 5u);
}

// segments is attacker-controlled. It may end iteration early; it must never
// let the reader walk past the frame.
GE_TEST(proto_snake_delta_rejects_lying_segment_count) {
    const std::size_t n = build_delta(g_buf, sizeof g_buf, 2, 4);
    auto* first = reinterpret_cast<SnakeRecord*>(
        g_buf + sizeof(FrameHeader) + sizeof(SnakeDeltaHeader));
    first->segments = 60000;

    FrameReader r(std::span<const std::byte>(g_buf, n));
    FrameView f;
    CHECK(r.next(f) == ParseError::Ok);        // the frame itself is well formed

    SnakeDeltaView v(f);
    const SnakeRecord* rec = nullptr;
    std::span<const Point2> pts;
    int seen = 0;
    while (v.next(rec, pts)) ++seen;
    CHECK_EQ(seen, 0);                          // the lie simply ends iteration
}

GE_TEST(proto_snake_delta_survives_truncation) {
    const std::size_t n = build_delta(g_buf, sizeof g_buf, 4, 6);
    for (std::size_t cut = sizeof(FrameHeader) + sizeof(SnakeDeltaHeader); cut < n; cut += 8) {
        FrameView f(g_buf, std::uint16_t(cut - sizeof(FrameHeader)));
        SnakeDeltaView v(f);
        const SnakeRecord* rec = nullptr;
        std::span<const Point2> pts;
        while (v.next(rec, pts)) {
            const auto* raw = reinterpret_cast<const std::byte*>(pts.data());
            CHECK(raw + pts.size() * sizeof(Point2) <= g_buf + cut);
        }
    }
}

GE_TEST(proto_snake_delta_writer_stops_when_frame_is_full) {
    alignas(8) std::byte small[256];
    SnakeDeltaWriter w(small, 1, 1);
    int committed = 0;
    for (int i = 0; i < 100; ++i) {
        auto pts = w.begin_snake(20);
        if (pts.empty()) break;
        SnakeRecord rec{};
        w.commit_snake(rec, 20);
        ++committed;
    }
    const std::size_t n = w.finish(SnakeDeltaHeader{});
    CHECK(committed > 0);
    CHECK(n <= sizeof small);                   // never wrote past the buffer
    CHECK_EQ(w.count(), std::uint16_t(committed));
}

GE_TEST(proto_type_mismatch_returns_null) {
    const std::size_t n = encode(g_buf, Ping{1, 2}, 1, 1);
    FrameReader r(std::span<const std::byte>(g_buf, n));
    FrameView f;
    CHECK(r.next(f) == ParseError::Ok);
    CHECK(f.as<Ping>() != nullptr);
    CHECK(f.as<MoveCommand>() == nullptr);        // the tag guards the cast
    CHECK(f.as<ClientHello>() == nullptr);
}

GE_TEST(proto_streams_multiple_frames) {
    std::size_t off = 0;
    for (int i = 0; i < 5; ++i)
        off += encode(std::span<std::byte>(g_buf + off, sizeof g_buf - off),
                      Ping{std::uint64_t(i), 0}, 1, std::uint32_t(i));

    FrameReader r(std::span<const std::byte>(g_buf, off));
    FrameView f;
    for (int i = 0; i < 5; ++i) {
        CHECK(r.next(f) == ParseError::Ok);
        CHECK_EQ(f.as<Ping>()->client_ts_ns, std::uint64_t(i));
    }
    CHECK(r.next(f) == ParseError::Incomplete);
    CHECK_EQ(r.consumed(), off);
}

GE_TEST(proto_partial_frame_is_incomplete_not_an_error) {
    const std::size_t n = encode(g_buf, Ping{1, 2}, 1, 1);
    for (std::size_t cut = 1; cut < n; ++cut) {
        FrameReader r(std::span<const std::byte>(g_buf, cut));
        FrameView f;
        CHECK(r.next(f) == ParseError::Incomplete);
        CHECK_EQ(r.consumed(), 0u);
    }
}

GE_TEST(proto_rejects_bad_magic) {
    encode(g_buf, Ping{1, 2}, 1, 1);
    const std::uint32_t bad = 0xBADF00D;
    std::memcpy(g_buf, &bad, sizeof bad);
    FrameReader r(std::span<const std::byte>(g_buf, sizeof(FrameHeader) + sizeof(Ping)));
    FrameView f;
    CHECK(r.next(f) == ParseError::BadMagic);
}

// Misaligned lengths are rejected, because accepting one would leave the *next*
// frame unaligned and break the invariant the zero-copy cast relies on.
GE_TEST(proto_rejects_unaligned_length) {
    encode(g_buf, Ping{1, 2}, 1, 1);
    const std::uint16_t odd = 17;
    std::memcpy(g_buf + 4, &odd, sizeof odd);
    FrameReader r(std::span<const std::byte>(g_buf, sizeof g_buf));
    FrameView f;
    CHECK(r.next(f) == ParseError::BadLength);
}

GE_TEST(proto_rejects_length_shorter_than_type_requires) {
    encode(g_buf, MoveCommand{}, 1, 1);
    const std::uint16_t too_short = 8;            // valid alignment, wrong size for MoveCommand
    std::memcpy(g_buf + 4, &too_short, sizeof too_short);
    FrameReader r(std::span<const std::byte>(g_buf, sizeof g_buf));
    FrameView f;
    CHECK(r.next(f) == ParseError::BadLength);
}

GE_TEST(proto_rejects_unknown_type) {
    encode(g_buf, Ping{1, 2}, 1, 1);
    g_buf[6] = std::byte(200);
    FrameReader r(std::span<const std::byte>(g_buf, sizeof(FrameHeader) + sizeof(Ping)));
    FrameView f;
    CHECK(r.next(f) == ParseError::BadType);
}

GE_TEST(proto_rejects_oversized_payload) {
    encode(g_buf, Ping{1, 2}, 1, 1);
    const std::uint16_t huge = 60000;
    std::memcpy(g_buf + 4, &huge, sizeof huge);
    FrameReader r(std::span<const std::byte>(g_buf, sizeof g_buf));
    FrameView f;
    CHECK(r.next(f) == ParseError::TooLarge);
}

// Random bytes must never produce a read outside the buffer, an infinite loop,
// or a crash. The parser is the only code that touches untrusted input, so it
// is the only code that has to survive arbitrary input.
GE_TEST(proto_fuzz_never_reads_out_of_bounds) {
    core::Xoshiro256ss rng(0xF00D);
    std::vector<std::byte> buf(1024);
    int accepted = 0, rejected = 0;

    for (int iter = 0; iter < 20'000; ++iter) {
        const std::size_t len = std::size_t(rng.next_below(1000) / 8) * 8;   // keep the base 8-aligned
        for (std::size_t i = 0; i < len; ++i) buf[i] = std::byte(rng.next() & 0xFF);
        // Half the time, plant a valid magic so the parser gets past the first gate.
        if (len >= sizeof(FrameHeader) && (rng.next() & 1)) {
            const std::uint32_t m = kMagic;
            std::memcpy(buf.data(), &m, sizeof m);
        }
        FrameReader r(std::span<const std::byte>(buf.data(), len));
        FrameView f;
        for (int step = 0; step < 64; ++step) {
            const ParseError e = r.next(f);
            if (e == ParseError::Ok) {
                ++accepted;
                // Anything accepted must lie entirely inside the buffer.
                const auto bytes = f.bytes();
                CHECK(bytes.data() >= buf.data());
                CHECK(bytes.data() + bytes.size() <= buf.data() + len);
                visit(f, [](const auto&, auto...) {});   // must not fault
            } else {
                ++rejected;
                break;
            }
        }
    }
    CHECK(rejected > 0);
    (void)accepted;
}

GE_TEST(proto_visit_dispatches_to_the_right_type) {
    int pings = 0, moves = 0, deltas = 0, entities = 0;
    auto handler = [&](const auto& msg, auto... tail) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, Ping>) ++pings;
        else if constexpr (std::is_same_v<T, MoveCommand>) ++moves;
        else if constexpr (std::is_same_v<T, SnakeDeltaHeader>) {
            ++deltas;
            entities += int(msg.count);
            (void)sizeof...(tail);
        }
    };

    std::size_t n = encode(g_buf, Ping{1, 2}, 1, 1);
    FrameReader r1(std::span<const std::byte>(g_buf, n));
    FrameView f;
    r1.next(f); visit(f, handler);

    n = encode(g_buf, MoveCommand{}, 1, 1);
    FrameReader r2(std::span<const std::byte>(g_buf, n));
    r2.next(f); visit(f, handler);

    n = build_delta(g_buf, sizeof g_buf, 3, 4);
    FrameReader r3(std::span<const std::byte>(g_buf, n));
    r3.next(f); visit(f, handler);

    CHECK_EQ(pings, 1);
    CHECK_EQ(moves, 1);
    CHECK_EQ(deltas, 1);
    CHECK_EQ(entities, 3);
}

GE_TEST(proto_encode_refuses_undersized_output) {
    std::byte small[8];
    CHECK_EQ(encode(small, Ping{1, 2}, 1, 1), 0u);
}

// ------------------------------------------------- arena delta: two tails
// The ShooterDelta tail is `count` PlayerRecords followed by `pickup_count`
// PickupRecords. Two flat arrays in one payload is the only place in this
// protocol where a reader has to derive one offset from another, so it is the
// only place an attacker-supplied count can move a pointer.

GE_TEST(shooter_delta_tail_splits_players_from_pickups) {
    PlayerRecord ps[3]{};
    for (int i = 0; i < 3; ++i) {
        ps[i].id   = std::uint32_t(i + 1);
        ps[i].x    = float(i);
        ps[i].health = 100;
        ps[i].fuel = std::uint8_t(10 * i);
        ps[i].ammo = std::uint8_t(i + 1);
    }
    PickupRecord ks[4]{};
    for (int i = 0; i < 4; ++i) {
        ks[i].id    = std::uint16_t(i);
        ks[i].x     = float(i * 2);
        ks[i].kind  = std::uint8_t(i % 2);
        ks[i].flags = std::uint8_t(i == 2 ? 0 : kPickupActive);
    }

    ShooterDeltaHeader h{};
    h.tick = 77; h.count = 3; h.pickup_count = 4;
    const std::size_t n = encode_flat2<ShooterDeltaHeader, PlayerRecord, PickupRecord>(
        g_buf, h, std::span<const PlayerRecord>(ps, 3),
        std::span<const PickupRecord>(ks, 4), 5, 9);
    CHECK_EQ(n, sizeof(FrameHeader) + sizeof(ShooterDeltaHeader)
                + 3 * sizeof(PlayerRecord) + 4 * sizeof(PickupRecord));

    FrameReader r({g_buf, n});
    FrameView f;
    CHECK(r.next(f) == ParseError::Ok);
    const ShooterDeltaView d(f);
    CHECK(d.valid());
    CHECK_EQ(d.header().tick, 77u);
    CHECK_EQ(d.players().size(), std::size_t(3));
    CHECK_EQ(d.pickups().size(), std::size_t(4));
    // The two bytes that replaced PlayerRecord::pad_ survive at the right
    // offset. Reading them from the wrong one yields plausible small integers,
    // which is exactly why this is checked by value and not by eye.
    CHECK_EQ(d.players()[2].fuel, std::uint8_t(20));
    CHECK_EQ(d.players()[2].ammo, std::uint8_t(3));
    // And the pickup array starts after the LAST player rather than somewhere
    // that happens to decode without crashing.
    CHECK_EQ(d.pickups()[3].x, 6.0f);
    CHECK((d.pickups()[2].flags & kPickupActive) == 0);
    CHECK((d.pickups()[3].flags & kPickupActive) != 0);
}

GE_TEST(shooter_delta_view_clamps_counts_it_cannot_trust) {
    PlayerRecord ps[2]{};
    ps[0].id = 1; ps[1].id = 2;
    PickupRecord k{};
    k.id = 9; k.x = 123.0f; k.flags = kPickupActive;

    // Physically two players and one pickup; the header claims ten players.
    // The pickup's offset therefore has to come from the CLAMPED player count
    // (2 -> 64 bytes) and not the declared one (10 -> 320 bytes), which would
    // point well past the end of the frame.
    ShooterDeltaHeader h{};
    h.count = 10; h.pickup_count = 1;
    const std::size_t n = encode_flat2<ShooterDeltaHeader, PlayerRecord, PickupRecord>(
        g_buf, h, std::span<const PlayerRecord>(ps, 2),
        std::span<const PickupRecord>(&k, 1), 1, 1);

    FrameReader r({g_buf, n});
    FrameView f;
    CHECK(r.next(f) == ParseError::Ok);
    const ShooterDeltaView d(f);
    CHECK(d.valid());
    CHECK_EQ(d.players().size(), std::size_t(2));   // clamped, not 10
    CHECK_EQ(d.pickups().size(), std::size_t(1));
    CHECK_EQ(d.pickups()[0].x, 123.0f);             // found where it really is
}

GE_TEST(shooter_delta_view_survives_a_header_only_payload) {
    ShooterDeltaHeader h{};
    h.tick = 5; h.count = 60000; h.pickup_count = 60000;
    const std::size_t n = encode_flat2<ShooterDeltaHeader, PlayerRecord, PickupRecord>(
        g_buf, h, std::span<const PlayerRecord>{}, std::span<const PickupRecord>{}, 1, 1);

    FrameReader r({g_buf, n});
    FrameView f;
    CHECK(r.next(f) == ParseError::Ok);
    const ShooterDeltaView d(f);
    // A valid header with a wholly absent tail is not an error -- it is a tick
    // on which nothing was visible. Both spans must simply be empty.
    CHECK(d.valid());
    CHECK_EQ(d.header().tick, 5u);
    CHECK(d.players().empty());
    CHECK(d.pickups().empty());
}

GE_TEST(a_worst_case_arena_delta_still_fits_one_frame) {
    // kMaxFrameBytes is what every downstream buffer is sized from at startup.
    // If the powerup array pushed the arena's worst case past it, the failure
    // would be a shard that silently sheds every delta rather than a crash.
    PlayerRecord ps[kMaxAoiPlayers]{};
    PickupRecord ks[kMaxPickups]{};
    alignas(8) std::byte big[kMaxFrameBytes];

    ShooterDeltaHeader h{};
    h.count = std::uint16_t(kMaxAoiPlayers);
    h.pickup_count = std::uint16_t(kMaxPickups);
    const std::size_t n = encode_flat2<ShooterDeltaHeader, PlayerRecord, PickupRecord>(
        big, h, std::span<const PlayerRecord>(ps, kMaxAoiPlayers),
        std::span<const PickupRecord>(ks, kMaxPickups), 1, 1);
    CHECK(n > 0);                                   // zero means it did not fit
    CHECK_EQ(n, kMaxArenaFrameBytes);
    CHECK(n <= kMaxFrameBytes);
}

// ---------------------------------------------------------------------------
// Mutation fuzzing of the frame reader.
//
// The codec casts structs in place instead of parsing field by field, so the
// checks in FrameReader are all that stands between a hostile datagram and a
// wild read. The hand-written malformed cases above cover what I thought of.
// These cover what I did not: valid frames with bytes flipped, and pure random
// input. Under ASan/UBSan an out-of-bounds read fails the run.
//
// Seeded, so a failure reproduces exactly.
// ---------------------------------------------------------------------------
namespace {

struct FuzzRng {
    std::uint64_t s;
    std::uint64_t next() noexcept {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
    std::uint32_t below(std::uint32_t n) noexcept { return std::uint32_t(next() % n); }
};

// Walks every accessor a real receiver would touch, so the fuzzer exercises
// the tail readers and not just the header checks.
void consume(const FrameView& f) noexcept {
    switch (f.type()) {
        case MsgType::ClientHello:   (void)f.as<ClientHello>();   break;
        case MsgType::ServerWelcome: (void)f.as<ServerWelcome>(); break;
        case MsgType::Ping:          (void)f.as<Ping>();          break;
        case MsgType::Pong:          (void)f.as<Pong>();          break;
        case MsgType::ShooterInput:  (void)f.as<ShooterInput>();  break;
        case MsgType::ShooterDelta: {
            const ShooterDeltaView d(f);
            if (!d.valid()) break;
            std::uint64_t acc = 0;
            for (const PlayerRecord& r : d.players()) acc += r.id;
            for (const PickupRecord& r : d.pickups()) acc += r.id;
            (void)acc;
            break;
        }
        case MsgType::SnakeDelta: {
            SnakeDeltaView v(f);
            const SnakeRecord* sr = nullptr;
            std::span<const Point2> pts;
            float acc = 0;
            while (v.next(sr, pts)) for (const Point2& p : pts) acc += p.x;
            (void)acc;
            break;
        }
        default: break;
    }
}

// Copies into a heap block sized to the input EXACTLY, then parses that.
//
// Parsing out of a big fixed buffer is what this did first, and it made the
// whole exercise decoration: an over-read of a few bytes still lands inside
// the array, so ASan sees nothing. Proven by deliberately weakening the bounds
// check in FrameReader -- 32 other tests failed and all three fuzz tests
// passed. With an exact-sized allocation the redzone sits immediately after
// the last valid byte and the same sabotage is caught here.
void drain(const std::byte* src, std::size_t n) noexcept {
    if (n == 0) return;
    auto* exact = static_cast<std::byte*>(::operator new(n, std::align_val_t(8)));
    std::memcpy(exact, src, n);
    FrameReader r({exact, n});
    FrameView f;
    for (int guard = 0; guard < 512; ++guard) {
        if (r.next(f) != ParseError::Ok) break;
        consume(f);
    }
    ::operator delete(exact, std::align_val_t(8));
}

} // namespace

GE_TEST(proto_reader_survives_random_bytes) {
    FuzzRng rng{0xDEADBEEFCAFEULL};
    alignas(8) std::byte buf[2048];
    for (int it = 0; it < 20000; ++it) {
        const std::size_t n = (rng.below(sizeof buf) / 8) * 8;   // reader wants 8-byte alignment
        for (std::size_t i = 0; i < n; ++i) buf[i] = std::byte(rng.next() & 0xFF);
        drain(buf, n);
    }
    CHECK(true);   // the assertion is "did not crash or trip a sanitizer"
}

GE_TEST(proto_reader_survives_mutated_valid_frames) {
    FuzzRng rng{0x1234567890ABULL};
    alignas(8) std::byte buf[4096];

    for (int it = 0; it < 20000; ++it) {
        // Build a valid multi-frame buffer first: mutations of something that
        // nearly parses reach far deeper than random noise does.
        std::size_t off = 0;
        const int frames = 1 + int(rng.below(3));
        for (int k = 0; k < frames && off + 256 < sizeof buf; ++k) {
            std::span<std::byte> out(buf + off, sizeof buf - off);
            switch (rng.below(4)) {
                case 0: off += encode(out, Ping{rng.next(), rng.next()}, 1, 1); break;
                case 1: off += encode(out, ClientHello{rng.next(), kProtocolVersion, 0}, 0, 0); break;
                case 2: {
                    ShooterInput in{};
                    in.input_seq = std::uint32_t(rng.next());
                    in.client_ts_ns = rng.next();
                    off += encode(out, in, 1, 1);
                    break;
                }
                default: {
                    PlayerRecord recs[8]{};
                    PickupRecord picks[4]{};
                    const std::uint16_t np = std::uint16_t(1 + rng.below(8));
                    const std::uint16_t nk = std::uint16_t(rng.below(4));
                    ShooterDeltaHeader h{};
                    h.count = np; h.pickup_count = nk;
                    off += encode_flat2<ShooterDeltaHeader, PlayerRecord, PickupRecord>(
                        out, h, std::span<const PlayerRecord>(recs, np),
                        std::span<const PickupRecord>(picks, nk), 1, 1);
                    break;
                }
            }
        }
        if (off == 0) continue;

        // One to four byte flips. Low counts keep the frame nearly valid, which
        // is where the interesting length and count fields live.
        const int flips = 1 + int(rng.below(4));
        for (int k = 0; k < flips; ++k) {
            const std::size_t at = rng.below(std::uint32_t(off));
            buf[at] = std::byte(std::uint8_t(buf[at]) ^ (1u << rng.below(8)));
        }
        drain(buf, off);
    }
    CHECK(true);
}

GE_TEST(proto_reader_survives_truncation_at_every_offset) {
    alignas(8) std::byte buf[1024];
    PlayerRecord recs[6]{};
    PickupRecord picks[3]{};
    ShooterDeltaHeader h{};
    h.count = 6; h.pickup_count = 3;
    const std::size_t n = encode_flat2<ShooterDeltaHeader, PlayerRecord, PickupRecord>(
        buf, h, std::span<const PlayerRecord>(recs, 6),
        std::span<const PickupRecord>(picks, 3), 1, 1);
    CHECK(n > 0);
    // Every 8-byte prefix, including the empty one. A reader that trusts a
    // length field over the buffer it was handed fails here.
    for (std::size_t cut = 0; cut <= n; cut += 8) drain(buf, cut);
    CHECK(true);
}

// ---------------------------------------------------------------- join path
//
// The join handshake had two bugs that a unit test would have caught before a
// load test did: the client sent ClientHello once and never retried, and the
// server dropped a repeat hello from a peer it already knew. Both are
// round-trip properties of the protocol, not of the network, so they belong
// here. See docs/guide/13-known-defects.md.

GE_TEST(hello_round_trips_through_encode_and_decode) {
    alignas(8) std::byte buf[proto::kMaxFrameBytes];
    const proto::ClientHello sent{0xDEADBEEFCAFEF00Dull, proto::kProtocolVersion, 7};
    const std::size_t n = proto::encode(buf, sent, 0, 0, 0);
    GE_CHECK(n > 0);

    proto::FrameReader r({buf, n});
    proto::FrameView f;
    GE_CHECK(r.next(f) == proto::ParseError::Ok);
    GE_CHECK(f.type() == proto::MsgType::ClientHello);

    const auto* got = f.as<proto::ClientHello>();
    GE_CHECK(got != nullptr);
    GE_CHECK(got->nonce == sent.nonce);
    GE_CHECK(got->protocol == sent.protocol);
    GE_CHECK(got->region_hint == sent.region_hint);
}

GE_TEST(welcome_carries_what_a_client_cannot_deduce) {
    // A client that cannot read world_size, aoi and tick_hz off the welcome has
    // to guess them, and a wrong guess looks like an empty world rather than a
    // misconfiguration. Pin that they survive the round trip intact.
    alignas(8) std::byte buf[proto::kMaxFrameBytes];
    const std::uint32_t session = 4242;
    const proto::ServerWelcome sent{ge::core::mix64(session), session,
                                    2000.0f, 64.0f, 3, 8, 60, 0, 0};
    const std::size_t n = proto::encode(buf, sent, session, 0,
                                        std::uint8_t(proto::FrameFlags::ServerSent));
    GE_CHECK(n > 0);

    proto::FrameReader r({buf, n});
    proto::FrameView f;
    GE_CHECK(r.next(f) == proto::ParseError::Ok);
    GE_CHECK(f.type() == proto::MsgType::ServerWelcome);
    GE_CHECK(f.header().session == session);

    const auto* got = f.as<proto::ServerWelcome>();
    GE_CHECK(got != nullptr);
    GE_CHECK(got->session_id == session);
    GE_CHECK(got->session_key == ge::core::mix64(session));
    GE_CHECK(got->world_size == 2000.0f);
    GE_CHECK(got->aoi_radius == 64.0f);
    GE_CHECK(got->tick_hz == 60);
}

GE_TEST(a_repeated_hello_is_byte_identical_so_a_retry_is_idempotent) {
    // The client retries its hello on a timer. If two encodings of the same
    // hello differed, the server could not treat the second as a retry.
    alignas(8) std::byte a[proto::kMaxFrameBytes], b[proto::kMaxFrameBytes];
    const proto::ClientHello h{0x1234ull, proto::kProtocolVersion, 0};
    const std::size_t na = proto::encode(a, h, 0, 0, 0);
    const std::size_t nb = proto::encode(b, h, 0, 0, 0);
    GE_CHECK(na == nb);
    GE_CHECK(std::memcmp(a, b, na) == 0);
}
