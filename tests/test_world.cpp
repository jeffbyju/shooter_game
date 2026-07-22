#include "test.hpp"
#include <chrono>

#include <atomic>
#include <cmath>
#include <set>
#include <thread>
#include <vector>

#include "ge/core/bits.hpp"
#include "ge/proto/codec.hpp"
#include "ge/world/grid.hpp"
#include "ge/world/shard.hpp"

using namespace ge;
using namespace ge::world;

// A spatial index is only useful if it returns the same answer as the O(n^2)
// version it replaces. This checks that on random data, at random query points.
GE_TEST(grid_query_matches_brute_force) {
    constexpr int kN = 2'000;
    constexpr float kSize = 1000.0f, kRadius = 40.0f;

    core::Xoshiro256ss rng(99);
    std::vector<float> xs(kN), ys(kN);
    for (int i = 0; i < kN; ++i) {
        xs[std::size_t(i)] = float(rng.next_double()) * kSize;
        ys[std::size_t(i)] = float(rng.next_double()) * kSize;
    }

    SpatialGrid grid;
    grid.configure({0, 0, kSize, kSize}, 32.0f, kN);
    grid.rebuild(xs.data(), ys.data(), kN);

    for (int q = 0; q < 200; ++q) {
        const float cx = float(rng.next_double()) * kSize;
        const float cy = float(rng.next_double()) * kSize;

        std::set<std::uint32_t> expected;
        for (int i = 0; i < kN; ++i) {
            const float dx = xs[std::size_t(i)] - cx, dy = ys[std::size_t(i)] - cy;
            if (dx * dx + dy * dy <= kRadius * kRadius) expected.insert(std::uint32_t(i));
        }

        std::set<std::uint32_t> got;
        grid.query_aabb(cx, cy, kRadius, [&](std::span<const std::uint32_t> cell) {
            for (std::uint32_t i : cell) {
                const float dx = xs[i] - cx, dy = ys[i] - cy;
                if (dx * dx + dy * dy <= kRadius * kRadius) got.insert(i);
            }
        });
        CHECK(got == expected);
    }
}

GE_TEST(grid_indexes_every_entity_exactly_once) {
    constexpr int kN = 500;
    core::Xoshiro256ss rng(3);
    std::vector<float> xs(kN), ys(kN);
    for (int i = 0; i < kN; ++i) {
        xs[std::size_t(i)] = float(rng.next_double()) * 500.0f;
        ys[std::size_t(i)] = float(rng.next_double()) * 500.0f;
    }
    SpatialGrid grid;
    grid.configure({0, 0, 500, 500}, 25.0f, kN);
    grid.rebuild(xs.data(), ys.data(), kN);

    std::vector<int> seen(kN, 0);
    grid.query_aabb(250, 250, 10'000, [&](std::span<const std::uint32_t> cell) {
        for (std::uint32_t i : cell) ++seen[i];
    });
    for (int s : seen) CHECK_EQ(s, 1);
}

GE_TEST(grid_clamps_out_of_bounds_queries) {
    std::vector<float> xs{10.0f}, ys{10.0f};
    SpatialGrid grid;
    grid.configure({0, 0, 100, 100}, 10.0f, 1);
    grid.rebuild(xs.data(), ys.data(), 1);

    int found = 0;
    grid.query_aabb(-500, -500, 1'000, [&](std::span<const std::uint32_t> c) { found += int(c.size()); });
    CHECK_EQ(found, 1);
    found = 0;
    grid.query_aabb(9'999, 9'999, 5, [&](std::span<const std::uint32_t> c) { found += int(c.size()); });
    CHECK(found <= 1);
}
GE_TEST(snake_store_swap_pop_keeps_body_with_its_snake) {
    SnakeStore s;
    s.reserve(8);
    for (std::uint32_t i = 0; i < 5; ++i)
        s.add(i + 1, float(i * 10), float(i * 10), 0.0f, ClientLink{i + 100, i, 0, 0});
    CHECK_EQ(s.size(), 5u);

    // Each snake's body is a slab slice indexed by its own position, so a
    // swap-and-pop has to move the body too, not just the scalar fields.
    const proto::Point2 last_head = s.point(4, 0);
    const std::uint32_t moved = s.remove(1);
    CHECK_EQ(moved, 4u);
    CHECK_EQ(s.size(), 4u);
    CHECK_EQ(s.id()[1], 5u);                   // the last snake took slot 1
    CHECK_EQ(s.link()[1].session, 104u);
    CHECK_EQ(s.point(1, 0).x, last_head.x);    // ...and brought its body with it
    CHECK_EQ(s.point(1, 0).y, last_head.y);
}

GE_TEST(snake_body_is_a_ring_that_leaves_its_tail_behind) {
    SnakeStore s;
    s.reserve(4);
    const auto i = s.add(1, 0, 0, 0.0f, ClientLink{1, 0, 0, 0});
    s.length()[i] = 4;
    s.seed_body(i, 0, 0, 0.0f);
    CHECK_EQ(s.body_count()[i], 4u);

    // Push more points than the body holds: the head advances and the oldest
    // point falls out of the window rather than being shifted down.
    for (int k = 1; k <= 10; ++k) s.push_point(i, float(k), 0.0f);
    CHECK_EQ(s.body_count()[i], 4u);
    CHECK_EQ(s.point(i, 0).x, 10.0f);          // newest is the head
    CHECK_EQ(s.point(i, 1).x, 9.0f);
    CHECK_EQ(s.point(i, 3).x, 7.0f);           // oldest still in the window
}

GE_TEST(snake_growing_lets_the_body_extend) {
    SnakeStore s;
    s.reserve(4);
    const auto i = s.add(1, 0, 0, 0.0f, ClientLink{1, 0, 0, 0});
    s.length()[i] = 3;
    s.seed_body(i, 0, 0, 0.0f);
    CHECK_EQ(s.body_count()[i], 3u);

    s.length()[i] = 6;                          // ate something
    for (int k = 1; k <= 5; ++k) s.push_point(i, float(k), 0.0f);
    CHECK_EQ(s.body_count()[i], 6u);            // grew up to the new length
    CHECK(s.body_count()[i] <= kMaxLength);
}

namespace {
// Drives a shard exactly the way the server does, end to end, on one thread.
struct ShardFixture {
    WorldShard shard;
    std::atomic<bool> running{true};
    std::thread th;

    explicit ShardFixture(bool busy_poll = false, std::size_t out_ring_bytes = 1u << 20) {
        ShardConfig c;
        c.shard_id       = 0;
        c.tick_hz        = 200;                 // fast ticks keep the test short
        c.bounds         = {0, 0, 512, 512};
        c.cell_size      = 32;
        c.aoi_radius     = 1024;                // everyone sees everyone
        c.max_entities   = 256;
        c.io_threads     = 1;
        c.conns_per_io   = 64;
        c.busy_poll      = busy_poll;
        c.out_ring_bytes = out_ring_bytes;
        shard.configure(c);
        th = std::thread([this] { shard.run(running); });
    }
    ~ShardFixture() {
        running.store(false);
        if (th.joinable()) th.join();
    }

    static ShardCommand join(std::uint32_t session, std::uint32_t conn) {
        ShardCommand c{};
        c.kind = ShardCommand::Kind::Join;
        c.session = session; c.conn_id = conn;
        c.enqueue_ns = std::uint64_t(core::Clock::now_ns());
        return c;
    }
};

// Drains one snake delta out of the shard's outbox, returning false if empty.
bool take_delta(WorldShard& shard, proto::FrameView& f, world::OutRecord& hdr,
                std::vector<std::byte>& keep) {
    auto rec = shard.outbox(0).peek();
    if (rec.empty()) return false;
    std::memcpy(&hdr, rec.data(), sizeof hdr);
    keep.assign(rec.begin() + sizeof hdr, rec.begin() + sizeof hdr + hdr.frame_bytes);
    shard.outbox(0).pop();
    proto::FrameReader reader(std::span<const std::byte>(keep.data(), keep.size()));
    return reader.next(f) == proto::ParseError::Ok;
}
} // namespace

GE_TEST(shard_joins_simulates_and_emits_snake_deltas) {
    ShardFixture fx;
    CHECK(fx.shard.inbox(0).try_push(ShardFixture::join(7, 3)));

    ShardCommand move{};
    move.kind = ShardCommand::Kind::Move;
    move.session = 7; move.conn_id = 3;
    move.input_seq = 42; move.client_ts_ns = 0xCAFE;
    move.dx = 1.0f; move.dy = 0.0f;

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(fx.shard.inbox(0).try_push(move));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    int deltas = 0;
    bool saw_echo = false, saw_self = false, body_has_points = false;
    world::OutRecord hdr{};
    std::vector<std::byte> keep;
    proto::FrameView f;
    for (int i = 0; i < 64 && take_delta(fx.shard, f, hdr, keep); ++i) {
        CHECK_EQ(hdr.conn_id, 3u);
        CHECK(f.type() == proto::MsgType::SnakeDelta);
        CHECK_EQ(f.session(), 7u);

        const auto* h = f.as<proto::SnakeDeltaHeader>();
        CHECK(h != nullptr);
        if (h->echo_client_ts_ns == 0xCAFE) { saw_echo = true; CHECK_EQ(h->input_seq_ack, 42u); }

        proto::SnakeDeltaView v(f);
        const proto::SnakeRecord* sr = nullptr;
        std::span<const proto::Point2> pts;
        while (v.next(sr, pts))
            if (sr->flags & proto::kSnakeSelf) {
                saw_self = true;
                if (pts.size() >= kStartLength) body_has_points = true;
            }
        ++deltas;
    }
    CHECK(deltas > 0);
    CHECK(saw_echo);
    CHECK(saw_self);
    CHECK(body_has_points);          // a snake, not a point
    CHECK(fx.shard.counters().ticks > 0);
    CHECK_EQ(fx.shard.live_entities(), 1u);
}

GE_TEST(shard_leave_frees_the_snake_slot) {
    ShardFixture fx;
    for (std::uint32_t i = 0; i < 4; ++i)
        CHECK(fx.shard.inbox(0).try_push(ShardFixture::join(100 + i, i)));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK_EQ(fx.shard.live_entities(), 4u);

    ShardCommand l{};
    l.kind = ShardCommand::Kind::Leave;
    l.session = 101; l.conn_id = 1;
    CHECK(fx.shard.inbox(0).try_push(l));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK_EQ(fx.shard.live_entities(), 3u);

    // The swap-and-pop must not orphan the snake that moved: an input for the
    // relocated connection still has to find its snake.
    ShardCommand m{};
    m.kind = ShardCommand::Kind::Move;
    m.session = 103; m.conn_id = 3;
    m.input_seq = 9; m.client_ts_ns = 0xBEEF;
    m.dx = 0.0f; m.dy = 1.0f;
    CHECK(fx.shard.inbox(0).try_push(m));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    bool found = false;
    world::OutRecord hdr{};
    std::vector<std::byte> keep;
    proto::FrameView f;
    for (int i = 0; i < 256 && take_delta(fx.shard, f, hdr, keep); ++i)
        if (const auto* h = f.as<proto::SnakeDeltaHeader>())
            if (h->echo_client_ts_ns == 0xBEEF && f.session() == 103) found = true;
    CHECK(found);
}

// Two snakes driven head-on into each other: one must die and the other grow.
GE_TEST(shard_snakes_eat_each_other) {
    ShardFixture fx;
    for (std::uint32_t i = 0; i < 12; ++i)
        fx.shard.inbox(0).try_push(ShardFixture::join(200 + i, i));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Steer everyone at the middle of the map so collisions are guaranteed.
    for (int round = 0; round < 40; ++round) {
        for (std::uint32_t i = 0; i < 12; ++i) {
            ShardCommand m{};
            m.kind = ShardCommand::Kind::Move;
            m.session = 200 + i; m.conn_id = i;
            m.input_seq = std::uint32_t(round);
            // Aim roughly inward from wherever they are.
            const float ang = float(i) * 0.5236f + 3.14159f;
            m.dx = std::cos(ang); m.dy = std::sin(ang);
            fx.shard.inbox(0).try_push(m);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        // Keep the outbox drained so the shard is not shedding.
        while (!fx.shard.outbox(0).peek().empty()) fx.shard.outbox(0).pop();
    }

    const auto c = fx.shard.counters();
    CHECK(c.ticks > 10);
    CHECK(c.deaths > 0);                 // something died
    CHECK(c.deaths >= c.kills);          // a wall death has no killer
}

GE_TEST(shard_tick_rate_is_accurate) {
    ShardFixture fx;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto ticks = fx.shard.counters().ticks;
    // 200 Hz for ~0.5s. Allow generous slack: this runs on a loaded CI machine
    // alongside every other test, so the assertion is "the scheduler works",
    // not "the scheduler is precise" -- precision is reported, not asserted.
    CHECK(ticks > 50);
    CHECK(ticks < 200);
    CHECK_EQ(fx.shard.counters().ticks, fx.shard.tick_duration().count());
}

GE_TEST(shard_sheds_load_instead_of_blocking_when_output_backs_up) {
    // Nobody drains the outbox, so it fills. The shard must keep ticking and
    // count the drops rather than stalling. The ring is sized just above one
    // worst-case frame so the overflow happens within the test's lifetime --
    // and not below it, which would mean nothing could ever be written at all.
    ShardFixture fx(false, 1u << 16);
    for (std::uint32_t i = 0; i < 32; ++i)
        fx.shard.inbox(0).try_push(ShardFixture::join(500 + i, i));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto c = fx.shard.counters();
    CHECK(c.ticks > 10);                       // still ticking
    CHECK(c.deltas_built > 0);
    CHECK(c.deltas_dropped > 0);               // and shedding, visibly
}

GE_TEST(shard_command_ring_rejects_rather_than_grows) {
    WorldShard s;
    ShardConfig c;
    c.io_threads   = 1;
    c.cmd_capacity = 16;
    c.conns_per_io = 8;
    c.max_entities = 8;
    s.configure(c);

    int accepted = 0;
    ShardCommand cmd{};
    while (s.inbox(0).try_push(cmd)) ++accepted;
    CHECK_EQ(accepted, 16);                    // exactly the configured bound
    CHECK(!s.inbox(0).try_push(cmd));
}
