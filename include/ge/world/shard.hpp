#pragma once
// One simulation shard: a thread that owns a region of the world, ticks it at a
// fixed rate, and talks to the I/O threads only through lock-free rings.
//
// The concurrency model is "shared nothing by construction". A shard's entity
// arrays are touched by exactly one thread, so there is not a single mutex on
// the simulation path and no atomic read-modify-write in the tick body. What
// crosses threads is bounded and one-directional:
//
//   I/O thread  --SpscRing<ShardCommand>-->  shard     (fixed-size commands)
//   shard       --SpscByteRing------------>  I/O thread (variable-size frames,
//                                                        written in place)
//
// Both rings are bounded, so an overloaded shard sheds load and records it
// rather than growing a queue until the process dies. Every drop is counted:
// there is no silent failure mode here.
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "ge/alloc/arena.hpp"
#include "ge/container/spsc_byte_ring.hpp"
#include "ge/container/spsc_ring.hpp"
#include "ge/core/bits.hpp"
#include "ge/core/clock.hpp"
#include "ge/metrics/counters.hpp"
#include "ge/metrics/histogram.hpp"
#include "ge/proto/wire.hpp"
#include "ge/world/snake.hpp"
#include "ge/world/grid.hpp"

namespace ge::world {

// 48 bytes, one per client input. enqueue_ns is stamped by the I/O thread so
// the shard can report queue transit time separately from simulation time --
// without that split, "server latency" is one number you cannot act on.
struct ShardCommand {
    enum class Kind : std::uint8_t { Join = 0, Leave, Move };

    Kind          kind         = Kind::Move;
    std::uint8_t  pad_         = 0;
    std::uint16_t io_thread    = 0;
    std::uint32_t conn_id      = 0;
    std::uint32_t session      = 0;
    std::uint32_t input_seq    = 0;
    std::uint64_t client_ts_ns = 0;
    std::uint64_t enqueue_ns   = 0;
    float         dx           = 0;
    float         dy           = 0;
    std::uint32_t pad2_        = 0;
    std::uint32_t pad3_        = 0;
};
static_assert(sizeof(ShardCommand) == 48);
static_assert(std::is_trivially_copyable_v<ShardCommand>);

// Prefix on every outbound ring record: tells the I/O thread which connection
// the frame that follows belongs to. The frame itself is already wire-ready.
struct OutRecord {
    std::uint32_t conn_id;
    std::uint32_t frame_bytes;
};
static_assert(sizeof(OutRecord) == 8);

// Cross-thread wakeup without dragging a networking dependency into the world
// layer: a function pointer and a context, no allocation, no virtual call.
struct Waker {
    void (*fn)(void*) = nullptr;
    void* ctx         = nullptr;
    void operator()() const noexcept { if (fn) fn(ctx); }
};

struct ShardConfig {
    std::uint16_t shard_id      = 0;
    int           tick_hz       = 60;
    Bounds        bounds        {0, 0, 1000, 1000};
    float         cell_size     = 32.0f;
    float         aoi_radius    = 48.0f;
    // Ticks a dead snake waits before respawning. Long enough to register as a
    // death, short enough that a player is not sidelined.
    int           respawn_ticks = 30;
    std::size_t   max_entities  = 4096;
    std::size_t   cmd_capacity  = 8192;    // per I/O thread
    std::size_t   out_ring_bytes= 1u << 20;// per I/O thread
    std::uint16_t io_threads    = 1;
    std::uint32_t conns_per_io  = 4096;
    // Spin instead of sleeping between ticks. Costs a full core per shard and
    // buys roughly an order of magnitude off tick jitter; the benchmark reports
    // both so the trade is visible rather than assumed.
    bool          busy_poll     = false;
};

class WorldShard {
public:
    WorldShard() = default;
    WorldShard(const WorldShard&)            = delete;
    WorldShard& operator=(const WorldShard&) = delete;

    void configure(const ShardConfig& cfg);

    container::SpscRing<ShardCommand>& inbox(std::uint16_t io)  noexcept { return *inboxes_[io]; }
    container::SpscByteRing&           outbox(std::uint16_t io) noexcept { return *outboxes_[io]; }

    // Called once per tick for each I/O thread that received new output, so a
    // blocked poll() returns immediately instead of waiting out its timeout.
    // Without this the tick-to-wire latency would be quantised by the poll
    // timeout, which is the single easiest way to accidentally add milliseconds.
    void set_waker(std::uint16_t io, Waker w) noexcept { wakers_[io] = w; }

    // Runs until `running` goes false. Intended as a thread entry point.
    void run(std::atomic<bool>& running);

    const metrics::ShardCounters& counters()      const noexcept { return counters_; }
    const metrics::Histogram&     tick_jitter()   const noexcept { return tick_jitter_; }
    const metrics::Histogram&     tick_duration() const noexcept { return tick_duration_; }
    const metrics::Histogram&     queue_transit() const noexcept { return queue_transit_; }
    std::size_t                   live_entities() const noexcept { return snakes_.size(); }
    std::size_t                   memory_bytes()  const noexcept;

private:
    void apply_commands();
    void advance(float dt) noexcept;        // steer + move every snake
    void index_bodies() noexcept;           // rebuild the spatial index over body points
    void resolve_collisions() noexcept;     // heads against bodies: who eats whom
    void respawn_dead(std::uint64_t tick);
    void build_deltas(std::uint32_t tick);
    void spawn_at(std::uint32_t idx) noexcept;
    void wait_for(std::int64_t deadline_ns) noexcept;

    std::uint32_t entity_for(std::uint16_t io, std::uint32_t conn) const noexcept {
        return conn_index_[std::size_t(io) * cfg_.conns_per_io + conn];
    }
    void bind_entity(std::uint16_t io, std::uint32_t conn, std::uint32_t idx) noexcept {
        conn_index_[std::size_t(io) * cfg_.conns_per_io + conn] = idx;
    }

    static constexpr std::uint32_t kNoEntity = 0xFFFF'FFFFu;

    ShardConfig  cfg_{};
    SnakeStore   snakes_;
    SpatialGrid  grid_;

    // The spatial index is built over *body points*, not snakes: a snake is a
    // polyline and can span many cells, so the queryable unit has to be the
    // segment. These parallel arrays are the index's input and its reverse map.
    std::vector<float>         seg_x_, seg_y_;
    std::vector<std::uint32_t> seg_owner_;
    alloc::Arena scratch_;                       // per-tick, reset every tick

    std::vector<std::unique_ptr<container::SpscRing<ShardCommand>>> inboxes_;
    std::vector<std::unique_ptr<container::SpscByteRing>>           outboxes_;
    std::vector<std::uint32_t> conn_index_;      // (io, conn) -> entity index
    std::vector<std::uint32_t> aoi_scratch_;
    std::vector<Waker>         wakers_;
    std::vector<std::uint8_t>  dirty_io_;

    metrics::ShardCounters counters_{};
    metrics::Histogram     tick_jitter_{1'000'000'000ULL, 3};
    metrics::Histogram     tick_duration_{1'000'000'000ULL, 3};
    metrics::Histogram     queue_transit_{1'000'000'000ULL, 3};

    std::uint32_t next_entity_id_ = 1;
    core::Xoshiro256ss rng_{0x5A17};
};

} // namespace ge::world
