#include "ge/world/shard.hpp"
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "ge/core/thread.hpp"
#include "ge/proto/codec.hpp"

namespace ge::world {

void WorldShard::configure(const ShardConfig& cfg) {
    cfg_ = cfg;

    snakes_.reserve(cfg.max_entities);
    // The index holds every body point of every snake, so it is sized by
    // segments, not snakes.
    const std::size_t max_segments = cfg.max_entities * kMaxLength;
    grid_.configure(cfg.bounds, cfg.cell_size, max_segments);
    seg_x_.reserve(max_segments);
    seg_y_.reserve(max_segments);
    seg_owner_.reserve(max_segments);

    scratch_.reserve(cfg.max_entities * proto::kMaxAoiSnakes * sizeof(std::uint32_t) + (1u << 16));
    scratch_.prefault();

    inboxes_.reserve(cfg.io_threads);
    outboxes_.reserve(cfg.io_threads);
    for (std::uint16_t i = 0; i < cfg.io_threads; ++i) {
        inboxes_.push_back(std::make_unique<container::SpscRing<ShardCommand>>(cfg.cmd_capacity));
        outboxes_.push_back(std::make_unique<container::SpscByteRing>(cfg.out_ring_bytes));
    }

    // An outbox that cannot hold one worst-case frame can never transmit
    // anything at all: every begin_write fails and the shard sheds 100% while
    // reporting a perfectly healthy tick rate. Caught by a test that sized its
    // ring for the old, much smaller point-based delta.
    GE_CHECK(cfg.out_ring_bytes >= sizeof(OutRecord) + proto::kMaxFrameBytes);

    conn_index_.assign(std::size_t(cfg.io_threads) * cfg.conns_per_io, kNoEntity);
    aoi_scratch_.assign(proto::kMaxAoiSnakes, 0);
    wakers_.assign(cfg.io_threads, Waker{});
    dirty_io_.assign(cfg.io_threads, 0);
    rng_ = core::Xoshiro256ss(0x5A17u + cfg.shard_id);
}

std::size_t WorldShard::memory_bytes() const noexcept {
    std::size_t total = snakes_.bytes() + grid_.bytes() + scratch_.capacity()
                      + conn_index_.capacity() * sizeof(std::uint32_t)
                      + (seg_x_.capacity() + seg_y_.capacity()) * sizeof(float)
                      + seg_owner_.capacity() * sizeof(std::uint32_t)
                      + tick_jitter_.memory_bytes() + tick_duration_.memory_bytes()
                      + queue_transit_.memory_bytes();
    for (const auto& r : inboxes_)  total += r->capacity() * sizeof(ShardCommand);
    for (const auto& r : outboxes_) total += r->capacity();
    return total;
}

// Places a snake somewhere in the world, pointing somewhere. Used for a first
// join and for every respawn.
void WorldShard::spawn_at(std::uint32_t idx) noexcept {
    const float margin = cfg_.bounds.width() * 0.1f;
    const float x = cfg_.bounds.min_x + margin
                  + float(rng_.next_double()) * (cfg_.bounds.width()  - 2 * margin);
    const float y = cfg_.bounds.min_y + margin
                  + float(rng_.next_double()) * (cfg_.bounds.height() - 2 * margin);
    const float h = float(rng_.next_double() * 6.2831853);

    snakes_.heading()[idx]      = h;
    snakes_.want_heading()[idx] = h;
    snakes_.length()[idx]       = kStartLength;
    snakes_.alive()[idx]        = 1;
    snakes_.since_drop()[idx]   = 0.0f;
    snakes_.seed_body(idx, x, y, h);
}

void WorldShard::apply_commands() {
    const std::int64_t now = core::Clock::now_ns();

    for (std::uint16_t io = 0; io < cfg_.io_threads; ++io) {
        inboxes_[io]->drain([&](const ShardCommand& c) {
            ++counters_.commands;
            if (c.enqueue_ns)
                queue_transit_.record(std::uint64_t(std::max<std::int64_t>(0, now - std::int64_t(c.enqueue_ns))));

            switch (c.kind) {
                case ShardCommand::Kind::Join: {
                    if (snakes_.size() >= cfg_.max_entities) return;   // admission control
                    const auto idx = snakes_.add(next_entity_id_++, 0, 0, 0,
                                                 ClientLink{c.session, c.conn_id, c.io_thread, 0});
                    spawn_at(idx);
                    bind_entity(c.io_thread, c.conn_id, idx);
                    break;
                }
                case ShardCommand::Kind::Leave: {
                    const std::uint32_t idx = entity_for(c.io_thread, c.conn_id);
                    if (idx == kNoEntity) return;
                    const std::uint32_t moved = snakes_.remove(idx);
                    bind_entity(c.io_thread, c.conn_id, kNoEntity);
                    if (moved != idx && idx < snakes_.size()) {
                        const ClientLink& l = snakes_.link()[idx];
                        bind_entity(l.io_thread, l.conn_id, idx);
                    }
                    break;
                }
                case ShardCommand::Kind::Move: {
                    const std::uint32_t idx = entity_for(c.io_thread, c.conn_id);
                    if (idx == kNoEntity) return;
                    // The input is a direction vector; what it sets is the
                    // heading the snake will turn *towards*. A snake cannot stop
                    // or reverse instantly -- that is the whole game -- so the
                    // turn rate is applied in advance(), not here.
                    if (c.dx != 0.0f || c.dy != 0.0f)
                        snakes_.want_heading()[idx] = std::atan2(c.dy, c.dx);
                    snakes_.last_seq()[idx] = c.input_seq;
                    snakes_.last_ts()[idx]  = c.client_ts_ns;
                    break;
                }
            }
        });
    }
}

namespace {
// Shortest signed angle from a to b, in (-pi, pi].
GE_ALWAYS_INLINE float angle_delta(float a, float b) noexcept {
    float d = b - a;
    while (d >  3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    return d;
}
} // namespace

void WorldShard::advance(float dt) noexcept {
    const std::size_t n = snakes_.size();
    float* GE_RESTRICT x  = snakes_.x();
    float* GE_RESTRICT y  = snakes_.y();
    float* GE_RESTRICT hd = snakes_.heading();
    float* GE_RESTRICT wh = snakes_.want_heading();
    float* GE_RESTRICT sp = snakes_.speed();
    float* GE_RESTRICT sd = snakes_.since_drop();
    std::uint8_t* alive = snakes_.alive();

    const float max_turn = kTurnRate * dt;
    const float min_x = cfg_.bounds.min_x, max_x = cfg_.bounds.max_x;
    const float min_y = cfg_.bounds.min_y, max_y = cfg_.bounds.max_y;

    for (std::size_t i = 0; i < n; ++i) {
        if (!alive[i]) continue;

        // Steer towards the requested heading, rate limited.
        const float d = angle_delta(hd[i], wh[i]);
        hd[i] += std::clamp(d, -max_turn, max_turn);

        const float nx = x[i] + std::cos(hd[i]) * sp[i] * dt;
        const float ny = y[i] + std::sin(hd[i]) * sp[i] * dt;

        // Walls kill. Reflecting instead would be gentler, but a wall you can
        // die on is what makes the edges of the map matter.
        if (nx < min_x || nx > max_x || ny < min_y || ny > max_y) {
            alive[i] = 0;
            snakes_.respawn_at()[i] = std::int64_t(counters_.ticks) + cfg_.respawn_ticks;
            continue;
        }
        x[i] = nx; y[i] = ny;

        // A body point is dropped every kSegmentSpacing of travel, so the body
        // has a constant physical density regardless of speed or tick rate.
        sd[i] += sp[i] * dt;
        while (sd[i] >= kSegmentSpacing) {
            sd[i] -= kSegmentSpacing;
            snakes_.push_point(std::uint32_t(i), x[i], y[i]);
        }
    }
    counters_.entities_stepped += n;
}

void WorldShard::index_bodies() noexcept {
    const std::size_t n = snakes_.size();
    seg_x_.clear(); seg_y_.clear(); seg_owner_.clear();
    const std::uint8_t* alive = snakes_.alive();
    const std::uint16_t* bc   = snakes_.body_count();

    for (std::size_t i = 0; i < n; ++i) {
        if (!alive[i]) continue;
        for (std::uint16_t k = 0; k < bc[i]; ++k) {
            const proto::Point2 p = snakes_.point(std::uint32_t(i), k);
            seg_x_.push_back(p.x);
            seg_y_.push_back(p.y);
            seg_owner_.push_back(std::uint32_t(i));
        }
    }
    grid_.rebuild(seg_x_.data(), seg_y_.data(), seg_x_.size());
}

void WorldShard::resolve_collisions() noexcept {
    const std::size_t n = snakes_.size();
    const float* xs = snakes_.x();
    const float* ys = snakes_.y();
    std::uint8_t* alive = snakes_.alive();
    std::uint16_t* len  = snakes_.length();
    std::uint16_t* kills= snakes_.kills();
    const float r2 = kCollideRadius * kCollideRadius;

    // Deaths are decided against the state at the start of the pass, so two
    // snakes that run into each other on the same tick both die rather than the
    // lower-indexed one winning by accident of iteration order.
    for (std::size_t i = 0; i < n; ++i) {
        if (!alive[i]) continue;
        std::uint32_t killer = kNoEntity;

        grid_.query_aabb(xs[i], ys[i], kCollideRadius, [&](std::span<const std::uint32_t> cell) {
            for (std::uint32_t s : cell) {
                const std::uint32_t owner = seg_owner_[s];
                if (owner == i) continue;                   // a snake cannot eat itself
                const float dx = seg_x_[s] - xs[i];
                const float dy = seg_y_[s] - ys[i];
                if (dx * dx + dy * dy > r2) continue;
                // Head-to-head is decided by length; otherwise running into a
                // body is fatal to the one who ran into it.
                const bool head_on = (std::fabs(seg_x_[s] - xs[owner]) < 1e-3f &&
                                      std::fabs(seg_y_[s] - ys[owner]) < 1e-3f);
                if (head_on && len[owner] < len[i]) continue;   // we win this one
                killer = owner;
                return;
            }
        });

        if (killer != kNoEntity) {
            alive[i] = 0;
            snakes_.respawn_at()[i] = std::int64_t(counters_.ticks) + cfg_.respawn_ticks;
            ++counters_.deaths;
            if (alive[killer]) {
                const auto gain = std::uint16_t(float(len[i]) * kEatFraction);
                len[killer] = std::min<std::uint16_t>(kMaxLength, std::uint16_t(len[killer] + gain));
                ++kills[killer];
                ++counters_.kills;
            }
        }
    }
}

void WorldShard::respawn_dead(std::uint64_t tick) {
    const std::size_t n = snakes_.size();
    std::uint8_t* alive = snakes_.alive();
    const std::int64_t* at = snakes_.respawn_at();
    for (std::size_t i = 0; i < n; ++i)
        if (!alive[i] && std::int64_t(tick) >= at[i]) spawn_at(std::uint32_t(i));
}

void WorldShard::build_deltas(std::uint32_t tick) {
    const std::size_t n = snakes_.size();
    const float radius  = cfg_.aoi_radius;
    const float r2      = radius * radius;

    const float* xs = snakes_.x();
    const float* ys = snakes_.y();
    const std::uint32_t* ids   = snakes_.id();
    const ClientLink*    links = snakes_.link();
    const std::uint32_t* lseq  = snakes_.last_seq();
    const std::uint64_t* lts   = snakes_.last_ts();
    const std::uint16_t* len   = snakes_.length();
    const std::uint16_t* kills = snakes_.kills();
    const std::uint16_t* bc    = snakes_.body_count();
    const std::uint8_t*  alive = snakes_.alive();

    std::fill(dirty_io_.begin(), dirty_io_.end(), std::uint8_t{0});

    for (std::size_t i = 0; i < n; ++i) {
        const ClientLink& link = links[i];
        if (link.session == 0) continue;                    // not a player

        // Work out who is visible *before* reserving ring space, so the
        // reservation is the exact size of this frame rather than the
        // worst-case one. Reserving the worst case (every visible snake at full
        // length, ~9.6 KB) would let a 4 MB ring hold ~430 frames; reserving
        // what is actually needed, typically a tenth of that, holds ten times as
        // many. The AOI list is cheap and stack-resident, so computing it twice
        // is not worth avoiding.
        std::uint32_t seen[proto::kMaxAoiSnakes];
        std::uint16_t seen_n = 0;
        auto already = [&](std::uint32_t o) {
            for (std::uint16_t k = 0; k < seen_n; ++k) if (seen[k] == o) return true;
            return false;
        };

        // Self first, so the client always learns where it is even if the area
        // is crowded enough to fill the frame.
        seen[seen_n++] = std::uint32_t(i);

        grid_.query_aabb(xs[i], ys[i], radius, [&](std::span<const std::uint32_t> cell) {
            counters_.aoi_candidates += cell.size();
            for (std::uint32_t sg : cell) {
                if (seen_n >= proto::kMaxAoiSnakes) return;
                const std::uint32_t owner = seg_owner_[sg];
                if (already(owner)) continue;
                const float dx = seg_x_[sg] - xs[i];
                const float dy = seg_y_[sg] - ys[i];
                if (dx * dx + dy * dy > r2) continue;
                seen[seen_n++] = owner;
            }
        });

        std::uint32_t need = std::uint32_t(sizeof(OutRecord) + sizeof(proto::FrameHeader) +
                                           sizeof(proto::SnakeDeltaHeader));
        for (std::uint16_t k = 0; k < seen_n; ++k) {
            const std::uint32_t o = seen[k];
            const std::uint16_t segs = alive[o] ? std::min<std::uint16_t>(bc[o], kMaxLength) : 0;
            need += std::uint32_t(sizeof(proto::SnakeRecord) + segs * sizeof(proto::Point2));
        }

        container::SpscByteRing& out = *outboxes_[link.io_thread];
        std::byte* dst = out.begin_write(need);
        if (GE_UNLIKELY(dst == nullptr)) {
            // The I/O thread is behind. Dropping this tick's view is correct for
            // a state-replication protocol -- the next tick carries strictly
            // newer information -- and blocking would stall the whole shard for
            // one slow client.
            ++counters_.deltas_dropped;
            continue;
        }

        proto::SnakeDeltaWriter w({dst + sizeof(OutRecord), need - sizeof(OutRecord)},
                                  link.session, tick);

        for (std::uint16_t k = 0; k < seen_n; ++k) {
            const std::uint32_t o = seen[k];
            const std::uint16_t segs = alive[o] ? std::min<std::uint16_t>(bc[o], kMaxLength) : 0;
            std::span<proto::Point2> pts = w.begin_snake(segs);
            if (pts.empty() && segs > 0) break;             // frame full
            for (std::uint16_t g = 0; g < segs; ++g) pts[g] = snakes_.point(o, g);

            proto::SnakeRecord rec{};
            rec.id     = ids[o];
            rec.flags  = std::uint16_t((o == i ? proto::kSnakeSelf : 0) |
                                       (alive[o] ? 0 : proto::kSnakeDead));
            rec.length = len[o];
            rec.kills  = kills[o];
            w.commit_snake(rec, segs);
        }

        proto::SnakeDeltaHeader hdr{};
        hdr.echo_client_ts_ns = lts[i];
        hdr.tick              = tick;
        hdr.input_seq_ack     = lseq[i];

        const std::size_t frame_bytes = w.finish(hdr);
        const OutRecord rec{link.conn_id, std::uint32_t(frame_bytes)};
        std::memcpy(dst, &rec, sizeof rec);
        out.commit_write(std::uint32_t(sizeof(OutRecord) + frame_bytes));
        dirty_io_[link.io_thread] = 1;
        ++counters_.deltas_built;
    }

    for (std::uint16_t io = 0; io < cfg_.io_threads; ++io)
        if (dirty_io_[io]) wakers_[io]();
}

void WorldShard::wait_for(std::int64_t deadline_ns) noexcept {
    if (cfg_.busy_poll) {
        while (core::Clock::now_ns() < deadline_ns) core::cpu_relax();
        return;
    }
    // Hybrid wait: sleep most of the interval, then spin the last slice.
    //
    // nanosleep's wakeup is accurate to roughly a scheduler tick, so sleeping
    // all the way to the deadline costs hundreds of microseconds of jitter.
    // Sleeping to (deadline - 250us) and spinning the remainder gets jitter
    // down to the low microseconds while leaving the core available for the
    // other 99.9% of the period.
    constexpr std::int64_t kSpinSlack = 250'000;
    const std::int64_t now = core::Clock::now_ns();
    const std::int64_t sleep_ns = deadline_ns - now - kSpinSlack;
    if (sleep_ns > 0) {
        timespec ts{sleep_ns / 1'000'000'000, long(sleep_ns % 1'000'000'000)};
        ::nanosleep(&ts, nullptr);
    }
    while (core::Clock::now_ns() < deadline_ns) core::cpu_relax();
}

void WorldShard::run(std::atomic<bool>& running) {
    char name[32];
    std::snprintf(name, sizeof name, "ge-shard-%u", unsigned(cfg_.shard_id));
    core::set_thread_name(name);
    core::request_perf_core(true);

    const core::TickScheduler sched(cfg_.tick_hz, core::Clock::now_ns());
    const float dt = 1.0f / float(cfg_.tick_hz);
    std::uint64_t tick = 0;

    while (running.load(std::memory_order_relaxed)) {
        const std::int64_t deadline = sched.deadline_of(tick);
        wait_for(deadline);

        const std::int64_t t0 = core::Clock::now_ns();
        tick_jitter_.record(std::uint64_t(std::max<std::int64_t>(0, t0 - deadline)));
        if (t0 > deadline + sched.period_ns() / 2) ++counters_.ticks_late;

        scratch_.reset();                      // the only "deallocation" in the loop

        apply_commands();
        respawn_dead(tick);
        advance(dt);
        index_bodies();
        resolve_collisions();
        const std::int64_t t1 = core::Clock::now_ns();

        build_deltas(std::uint32_t(tick));
        const std::int64_t t2 = core::Clock::now_ns();

        counters_.sim_ns   += std::uint64_t(t1 - t0);
        counters_.build_ns += std::uint64_t(t2 - t1);
        counters_.arena_high_water = scratch_.high_water();
        tick_duration_.record(std::uint64_t(t2 - t0));
        ++counters_.ticks;

        // If a tick overran its budget, skip the ticks we missed instead of
        // trying to catch up: replaying stale simulation steps back-to-back
        // would push the deadline further out and the loop would never recover.
        ++tick;
        const std::uint64_t behind = sched.catch_up(tick, t2);
        if (behind) {
            tick += behind;
            counters_.ticks_skipped += behind;
        }
    }
}

} // namespace ge::world
