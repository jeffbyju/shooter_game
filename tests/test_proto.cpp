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
