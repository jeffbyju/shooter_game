#pragma once
// Arena storage: the 3D shooter's entity model. Same rules as snake.hpp --
// structure of arrays, nothing allocated on the tick path -- but a player is
// just a point with a velocity and two angles.
//
// The world is a box: x,z in [0,size], y up. The floor is at 0 unless a
// Terrain is supplied to step(), in which case it is terrain->height(x,z).
// Still no level geometry beyond that -- no walls, no BSP -- because this mode
// exists to exercise the transport and a BSP would not change that.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ge/core/bits.hpp"       // Xoshiro256ss
#include "ge/core/platform.hpp"
#include "ge/proto/wire.hpp"
#include "ge/world/snake.hpp"    // ClientLink
#include "ge/world/terrain.hpp"

namespace ge::world {

inline constexpr float kEyeHeight     = 1.70f;   // camera offset above the feet
inline constexpr float kTorsoHeight   = 1.00f;   // centre of the hit sphere
inline constexpr float kHitRadius     = 0.55f;
inline constexpr float kMoveSpeed     = 9.0f;    // units per second
inline constexpr float kGravity       = -24.0f;
inline constexpr float kJumpSpeed     = 8.0f;
inline constexpr float kFireCooldown  = 0.12f;   // seconds between shots
inline constexpr float kMaxRange      = 400.0f;
inline constexpr std::uint16_t kMaxHealth   = 100;
inline constexpr std::uint16_t kHitDamage   = 25;
inline constexpr float kPitchLimit    = 1.5533f; // ~89 degrees

// ----------------------------------------------------------------- jetpack
// Thrust is an ACCELERATION and gravity keeps applying underneath it, rather
// than the jetpack setting a climb rate directly. That is what makes releasing
// the key feel like releasing a thruster instead of switching off a lift: the
// net upward acceleration is kJetAccel - |kGravity| = 18 m/s^2 while held, and
// -24 m/s^2 the instant it is not.
inline constexpr float kMaxFuel          = 100.0f;
inline constexpr float kJetAccel         = 42.0f;   // must exceed |kGravity| to climb at all
inline constexpr float kJetFuelPerSec    = 34.0f;   // ~2.9 s of continuous flight from full
inline constexpr float kJetMaxRise       = 11.0f;   // clamp on vy while thrusting
inline constexpr float kCeiling          = 45.0f;   // soft roof; see step()
// A slow trickle while standing still, and only while standing still. Without
// it a player who burns a full tank with no fuel powerup nearby is grounded
// permanently, which is a dead end rather than a cost. At 3/s a full refill by
// standing around takes 33 s against the 1.6 s a powerup takes to walk to, so
// the powerup is still the reason to move.
inline constexpr float kGroundRegenPerSec = 3.0f;

// The two balance facts the jetpack rests on, checked by the compiler rather
// than by playing the game and noticing. Thrust that does not exceed gravity
// is a jetpack that burns fuel and sinks; a jump faster than the rise clamp
// would be silently capped by a limit written for the jetpack.
static_assert(kJetAccel + kGravity > 0.0f,
              "jetpack thrust must exceed gravity or holding it does nothing but drain fuel");
static_assert(kJumpSpeed < kJetMaxRise,
              "the jetpack's rise clamp would also clip an ordinary jump");

// ------------------------------------------------------------------- ammo
inline constexpr std::uint16_t kMaxAmmo  = 30;
// 30 rounds at one per kFireCooldown is 3.6 s of held fire. Long enough that
// ammo is not a constant interruption, short enough that a fight has a reload
// problem in it.
inline constexpr std::uint16_t kAmmoPickupAmount = 18;
inline constexpr float         kFuelPickupAmount = 55.0f;
inline constexpr float         kPickupRadius     = 1.6f;   // generous: this is not a precision test
inline constexpr float         kPickupVertical   = 2.2f;   // half-height of the collection cylinder
inline constexpr float         kPickupY          = 1.0f;   // floats at chest height
inline constexpr int           kPickupRespawnSec = 9;

// A fixed pool of powerup sites. Not a container that grows and shrinks: the
// count is constant, the wire format sends all of them every tick, and a
// collected pickup is an inactive slot waiting on a timer rather than an
// erased element. That removes allocation from the tick path and makes the
// delta a fixed size, both of which the rest of this server already assumes.
class PickupField {
public:
    // Half fuel, half ammo, and the split is FIXED per slot rather than rolled
    // on each respawn. Rolling it is the obvious design and the wrong one:
    // with twelve slots an independent 50/50 per respawn produces stretches
    // where every powerup on the map is ammo, and a player out of fuel has
    // nothing to walk toward. Fixing the kind guarantees six of each are
    // always in rotation. The POSITION is what is random, which is the part
    // that makes a powerup worth hunting for.
    void init(std::uint16_t n, float world, core::Xoshiro256ss& rng,
              const Terrain* terrain = nullptr) {
        kind_.assign(n, 0); x_.assign(n, 0.0f); y_.assign(n, kPickupY);
        z_.assign(n, 0.0f); active_.assign(n, 1); respawn_at_.assign(n, 0);
        for (std::uint16_t i = 0; i < n; ++i) {
            kind_[i] = std::uint8_t(i % 2 == 0 ? proto::PickupKind::Fuel
                                               : proto::PickupKind::Ammo);
            place(i, world, rng, terrain);
        }
    }

    std::uint16_t size()   const noexcept { return std::uint16_t(kind_.size()); }
    bool  active(std::uint16_t i) const noexcept { return active_[i] != 0; }
    float x(std::uint16_t i) const noexcept { return x_[i]; }
    float y(std::uint16_t i) const noexcept { return y_[i]; }
    float z(std::uint16_t i) const noexcept { return z_[i]; }
    std::uint8_t kind(std::uint16_t i) const noexcept { return kind_[i]; }

    void take(std::uint16_t i, std::uint64_t tick, std::uint64_t respawn_ticks) noexcept {
        active_[i]     = 0;
        respawn_at_[i] = tick + respawn_ticks;
    }

    // Respawns anything whose timer has run out, somewhere new. Re-rolling the
    // position rather than restoring the old one is what stops the map turning
    // into a set of memorised camping spots.
    void update(std::uint64_t tick, float world, core::Xoshiro256ss& rng,
                const Terrain* terrain = nullptr) noexcept {
        for (std::uint16_t i = 0; i < size(); ++i) {
            if (active_[i] || tick < respawn_at_[i]) continue;
            place(i, world, rng, terrain);
            active_[i] = 1;
        }
    }

private:
    // Terrain is taken here rather than applied afterwards because respawn
    // re-rolls the position, and a pickup placed at a flat kPickupY would sink
    // into the first hill it respawned under -- invisible, and uncollectable
    // because the pickup radius is smaller than the hill.
    void place(std::uint16_t i, float world, core::Xoshiro256ss& rng,
               const Terrain* terrain) noexcept {
        const float margin = 6.0f;
        const float span   = world - 2.0f * margin;
        x_[i] = margin + float(rng.next_double()) * span;
        z_[i] = margin + float(rng.next_double()) * span;
        y_[i] = kPickupY + (terrain ? terrain->height(x_[i], z_[i]) : 0.0f);
    }

    std::vector<std::uint8_t>  kind_, active_;
    std::vector<float>         x_, y_, z_;
    std::vector<std::uint64_t> respawn_at_;
};

// A hitscan result. `victim` is a slot index, not an id.
struct HitResult {
    std::uint32_t victim = 0xffffffffu;
    float         t      = 0.0f;      // distance along the ray
    bool          hit    = false;
};

class ArenaStore {
public:
    void reserve(std::size_t n) {
        id_.reserve(n); x_.reserve(n); y_.reserve(n); z_.reserve(n);
        vy_.reserve(n); yaw_.reserve(n); pitch_.reserve(n);
        health_.reserve(n); score_.reserve(n); alive_.reserve(n);
        respawn_at_.reserve(n); cooldown_.reserve(n);
        fuel_.reserve(n); ammo_.reserve(n);
        in_x_.reserve(n); in_z_.reserve(n); in_btn_.reserve(n);
        link_.reserve(n); last_seq_.reserve(n); last_ts_.reserve(n);
        tick_flags_.reserve(n);
        cap_ = n;
    }

    std::uint32_t add(std::uint32_t id, float x, float z, ClientLink link) {
        const auto slot = std::uint32_t(id_.size());
        id_.push_back(id);
        x_.push_back(x); y_.push_back(0.0f); z_.push_back(z);
        vy_.push_back(0.0f); yaw_.push_back(0.0f); pitch_.push_back(0.0f);
        health_.push_back(kMaxHealth); score_.push_back(0); alive_.push_back(1);
        respawn_at_.push_back(0); cooldown_.push_back(0.0f);
        fuel_.push_back(kMaxFuel); ammo_.push_back(kMaxAmmo);
        in_x_.push_back(0.0f); in_z_.push_back(0.0f); in_btn_.push_back(0);
        link_.push_back(link); last_seq_.push_back(0); last_ts_.push_back(0);
        tick_flags_.push_back(0);
        return slot;
    }

    // Put a slot's feet on the terrain. add() and spawn() both place at y = 0,
    // which on terrain is usually inside a hill; step() would eject the player
    // upward on the next tick, but the first snapshot would already have gone
    // out with them buried. Called at spawn time so that never happens.
    void lift_to_ground(std::uint32_t slot, const Terrain& t) noexcept {
        y_[slot]  = t.height(x_[slot], z_[slot]);
        vy_[slot] = 0.0f;
    }

    void remove(std::uint32_t slot) noexcept {
        const auto last = id_.size() - 1;
        if (slot != last) {
            id_[slot]=id_[last]; x_[slot]=x_[last]; y_[slot]=y_[last]; z_[slot]=z_[last];
            vy_[slot]=vy_[last]; yaw_[slot]=yaw_[last]; pitch_[slot]=pitch_[last];
            health_[slot]=health_[last]; score_[slot]=score_[last];
            alive_[slot]=alive_[last]; respawn_at_[slot]=respawn_at_[last];
            cooldown_[slot]=cooldown_[last];
            fuel_[slot]=fuel_[last]; ammo_[slot]=ammo_[last];
            in_x_[slot]=in_x_[last]; in_z_[slot]=in_z_[last]; in_btn_[slot]=in_btn_[last];
            link_[slot]=link_[last]; last_seq_[slot]=last_seq_[last];
            last_ts_[slot]=last_ts_[last]; tick_flags_[slot]=tick_flags_[last];
        }
        id_.pop_back(); x_.pop_back(); y_.pop_back(); z_.pop_back();
        vy_.pop_back(); yaw_.pop_back(); pitch_.pop_back();
        health_.pop_back(); score_.pop_back(); alive_.pop_back();
        respawn_at_.pop_back(); cooldown_.pop_back();
        fuel_.pop_back(); ammo_.pop_back();
        in_x_.pop_back(); in_z_.pop_back(); in_btn_.pop_back();
        link_.pop_back(); last_seq_.pop_back(); last_ts_.pop_back();
        tick_flags_.pop_back();
    }

    std::size_t size() const noexcept { return id_.size(); }
    std::size_t capacity() const noexcept { return cap_; }

    // --- accessors used by the shard ---------------------------------------
    std::uint32_t id(std::uint32_t i)    const noexcept { return id_[i]; }
    float x(std::uint32_t i)             const noexcept { return x_[i]; }
    float y(std::uint32_t i)             const noexcept { return y_[i]; }
    float z(std::uint32_t i)             const noexcept { return z_[i]; }
    float yaw(std::uint32_t i)           const noexcept { return yaw_[i]; }
    float pitch(std::uint32_t i)         const noexcept { return pitch_[i]; }
    std::uint16_t health(std::uint32_t i)const noexcept { return health_[i]; }
    std::uint16_t score(std::uint32_t i) const noexcept { return score_[i]; }
    bool alive(std::uint32_t i)          const noexcept { return alive_[i] != 0; }
    float fuel(std::uint32_t i)          const noexcept { return fuel_[i]; }
    std::uint16_t ammo(std::uint32_t i)  const noexcept { return ammo_[i]; }
    // What goes on the wire: a percentage in one byte. Rounded rather than
    // truncated so a full tank reads as 100 and not 99.
    std::uint8_t fuel_pct(std::uint32_t i) const noexcept {
        const float f = fuel_[i] * (100.0f / kMaxFuel);
        return std::uint8_t(f < 0.0f ? 0.0f : (f > 100.0f ? 100.0f : f) + 0.5f);
    }
    std::uint16_t tick_flags(std::uint32_t i) const noexcept { return tick_flags_[i]; }
    // Raw spans of the position columns, for a spatial index that wants to
    // sweep them rather than call an accessor per entity. Handing out the
    // pointers is safe here for the same reason the SoA layout exists: the
    // arrays are only resized by add()/remove(), which no index outlives.
    const float* xs() const noexcept { return x_.data(); }
    const float* zs() const noexcept { return z_.data(); }

    const ClientLink& link(std::uint32_t i) const noexcept { return link_[i]; }
    std::uint32_t last_seq(std::uint32_t i) const noexcept { return last_seq_[i]; }
    std::uint64_t last_ts(std::uint32_t i)  const noexcept { return last_ts_[i]; }

    void clear_tick_flags() noexcept {
        for (auto& f : tick_flags_) f = 0;
    }

    // Latest input wins. Inputs are absolute, so replacing rather than
    // accumulating is correct and makes a dropped packet cost one stale tick.
    void set_input(std::uint32_t i, float mx, float mz, float yaw, float pitch,
                   std::uint32_t buttons, std::uint32_t seq, std::uint64_t ts) noexcept {
        in_x_[i] = mx; in_z_[i] = mz; in_btn_[i] = buttons;
        yaw_[i]  = yaw;
        pitch_[i] = pitch < -kPitchLimit ? -kPitchLimit
                  : (pitch > kPitchLimit ? kPitchLimit : pitch);
        last_seq_[i] = seq; last_ts_[i] = ts;
    }

    // A respawn restores fuel and ammo along with health. Spawning in empty
    // would make death cost a scavenging trip on top of the respawn timer,
    // which punishes the player who is already losing.
    void spawn(std::uint32_t i, float x, float z) noexcept {
        x_[i] = x; y_[i] = 0.0f; z_[i] = z; vy_[i] = 0.0f;
        health_[i] = kMaxHealth; alive_[i] = 1; cooldown_[i] = 0.0f;
        fuel_[i] = kMaxFuel; ammo_[i] = kMaxAmmo;
        tick_flags_[i] = 0;
    }

    // Integrate one tick. Movement is relative to yaw, gravity pulls toward the
    // floor, and the box is a hard clamp rather than a bounce.
    //
    // `terrain` is optional and defaults to flat ground at y=0, which is what
    // every existing caller and test gets. That is deliberate: it keeps the
    // flat world available as the control arm of an A/B rather than deleting
    // it, the same way `--aoi-scan` keeps the quadratic loop around.
    void step(float dt, float world_size, const Terrain* terrain = nullptr) noexcept {
        const std::size_t n = id_.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (!alive_[i]) continue;
            if (cooldown_[i] > 0.0f) cooldown_[i] -= dt;

            const float s = std::sin(yaw_[i]), c = std::cos(yaw_[i]);
            // Forward is +z rotated by yaw; strafe is the perpendicular.
            // forward = +z rotated by yaw; right = forward x worldUp = (-c, s).
            // Not (c, -s): that is the LEFT vector, and using it made the whole
            // coordinate frame left-handed. See gl_math.hpp for the derivation.
            const float fx = s,  fz = c;
            const float rx = -c,  rz = s;
            float dx = (fx * in_z_[i] + rx * in_x_[i]);
            float dz = (fz * in_z_[i] + rz * in_x_[i]);
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len > 1.0f) { dx /= len; dz /= len; }   // no diagonal speed bonus

            const float prev_x = x_[i], prev_z = z_[i];
            x_[i] += dx * kMoveSpeed * dt;
            z_[i] += dz * kMoveSpeed * dt;

            // --- ground ----------------------------------------------------
            // Everything below that used to compare against the constant 0 now
            // compares against `ground`. With no terrain that is 0 and the
            // arithmetic is identical to before.
            float ground = 0.0f;
            if (terrain) {
                const float prev_ground = terrain->height(prev_x, prev_z);
                ground = terrain->height(x_[i], z_[i]);
                // A slope you cannot climb is a wall you cannot walk through.
                // Compared as rise over run, not as an absolute rise: one tick
                // of walking covers only kMoveSpeed*dt, so an absolute limit
                // would need a near-vertical face to trigger and would never
                // fire on real ground.
                //
                // Gated on actually being near the ground, because a player
                // crossing the same cliff on a jetpack is above it and must not
                // be stopped. Testing altitude rather than a grounded flag
                // means a jump and a flight read the same way -- there is no
                // second definition of "on foot" to keep in sync.
                const float mx = x_[i] - prev_x, mz = z_[i] - prev_z;
                const float run = std::sqrt(mx * mx + mz * mz);
                if (run > 0.0f &&
                    y_[i] <= prev_ground + kEyeHeight &&
                    ground - prev_ground > kTerrainMaxSlope * run) {
                    x_[i]  = prev_x;
                    z_[i]  = prev_z;
                    ground = prev_ground;
                }
            }

            const bool grounded = y_[i] <= ground + 0.0001f && vy_[i] <= 0.0f;
            if (grounded && (in_btn_[i] & proto::kBtnJump)) vy_[i] = kJumpSpeed;

            // --- jetpack -------------------------------------------------
            // Fuel is the only gate. The client decides when to ASK for
            // thrust (the double tap), the server decides whether it can be
            // had, and a client that lies about the gesture still cannot fly
            // longer than its tank allows.
            const bool want_jet = (in_btn_[i] & proto::kBtnJet) != 0;
            const bool jetting  = want_jet && fuel_[i] > 0.0f;
            if (jetting) {
                fuel_[i] -= kJetFuelPerSec * dt;
                if (fuel_[i] < 0.0f) fuel_[i] = 0.0f;
                vy_[i] += kJetAccel * dt;
                tick_flags_[i] |= proto::kPlayerJet;
            // `!want_jet`, not merely `!jetting`. Holding the thruster on an
            // empty tank while standing on the floor otherwise regenerates a
            // sliver of fuel, spends it on one tick of thrust, lands, and
            // repeats -- a 60 Hz flutter that leaves the player microscopically
            // airborne forever and makes the tank take an unbounded time to
            // refill. Resting means letting go.
            } else if (!want_jet && grounded && fuel_[i] < kMaxFuel) {
                fuel_[i] += kGroundRegenPerSec * dt;
                if (fuel_[i] > kMaxFuel) fuel_[i] = kMaxFuel;
            }

            vy_[i] += kGravity * dt;
            // Clamped unconditionally rather than only while thrusting. A jump
            // leaves at kJumpSpeed = 8, well under this, so the clamp costs
            // nothing there -- and writing it as an unconditional property of
            // vertical speed means there is no second path to keep in sync.
            if (vy_[i] > kJetMaxRise) vy_[i] = kJetMaxRise;
            y_[i]  += vy_[i] * dt;
            if (y_[i] < ground) { y_[i] = ground; vy_[i] = 0.0f; }
            // Soft roof. Without one, a full tank buys about 33 m of altitude
            // and the arena below stops being legible -- players become dots
            // and the game turns into a staring contest. Hitting it kills the
            // climb but not the flight, so it reads as a ceiling rather than
            // as the jetpack failing.
            // Relative to the ground under the player, not to absolute zero.
            // On a 30-unit peak an absolute roof would leave 15 units of
            // headroom instead of 45, and on terrain taller than the roof it
            // would pin the player inside the hill.
            const float roof = ground + kCeiling;
            if (y_[i] > roof) { y_[i] = roof; if (vy_[i] > 0.0f) vy_[i] = 0.0f; }

            if (x_[i] < 0.0f) x_[i] = 0.0f;
            if (z_[i] < 0.0f) z_[i] = 0.0f;
            if (x_[i] > world_size) x_[i] = world_size;
            if (z_[i] > world_size) z_[i] = world_size;
        }
    }

    // Ammo is checked here rather than inside the firing code so that an empty
    // magazine produces no shot at all -- no cooldown, no tracer, no muzzle
    // flash on anyone else's screen. A dry trigger should look dry.
    bool wants_fire(std::uint32_t i) const noexcept {
        return alive_[i] && (in_btn_[i] & proto::kBtnFire)
            && cooldown_[i] <= 0.0f && ammo_[i] > 0;
    }

    // Hitscan from i's eye along its view direction. Nearest sphere wins.
    //
    // Spheres, not capsules: a capsule is the correct shape for a standing
    // human and costs a segment-segment distance per candidate. At this entity
    // count the difference is not measurable and a sphere keeps the test to
    // one dot product and a square root that is only taken on a hit.
    HitResult hitscan(std::uint32_t i) const noexcept {
        HitResult best;
        float best_t = kMaxRange;
        const float cp = std::cos(pitch_[i]), sp = std::sin(pitch_[i]);
        const float ox = x_[i], oy = y_[i] + kEyeHeight, oz = z_[i];
        const float dx = std::sin(yaw_[i]) * cp;
        const float dy = sp;
        const float dz = std::cos(yaw_[i]) * cp;

        const std::size_t n = id_.size();
        for (std::size_t j = 0; j < n; ++j) {
            if (j == i || !alive_[j]) continue;
            const float mx = x_[j] - ox;
            const float my = (y_[j] + kTorsoHeight) - oy;
            const float mz = z_[j] - oz;
            const float t  = mx * dx + my * dy + mz * dz;   // projection onto the ray
            if (t <= 0.0f || t >= best_t) continue;
            const float px = mx - dx * t, py = my - dy * t, pz = mz - dz * t;
            if (px * px + py * py + pz * pz > kHitRadius * kHitRadius) continue;
            best_t = t; best.victim = std::uint32_t(j); best.t = t; best.hit = true;
        }
        return best;
    }

    // One round leaves the barrel: start the cooldown, spend the round, and
    // flag the tick so clients draw a tracer. This was begin_cooldown(); it is
    // renamed because a function called that which silently decremented ammo
    // would be a trap for whoever added the next caller.
    void consume_shot(std::uint32_t i) noexcept {
        cooldown_[i] = kFireCooldown;
        if (ammo_[i] > 0) --ammo_[i];
        tick_flags_[i] |= proto::kPlayerFired;
    }

    // O(n*m) with m = 12, so 384 tests a tick at 32 players -- cheaper than
    // deciding whether to consult an index.
    //
    // A cylinder, not a sphere: reach should not shrink mid-jump. Skips players
    // already full, so a can is not absorbed and denied to the next person.
    void collect_pickups(PickupField& field, std::uint64_t tick,
                         std::uint64_t respawn_ticks) noexcept {
        const std::size_t    n = id_.size();
        const std::uint16_t  m = field.size();
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!alive_[i]) continue;
            for (std::uint16_t k = 0; k < m; ++k) {
                if (!field.active(k)) continue;
                const float dx = field.x(k) - x_[i];
                const float dz = field.z(k) - z_[i];
                if (dx * dx + dz * dz > kPickupRadius * kPickupRadius) continue;
                const float dy = field.y(k) - (y_[i] + kTorsoHeight);
                if (dy < -kPickupVertical || dy > kPickupVertical) continue;

                if (field.kind(k) == std::uint8_t(proto::PickupKind::Fuel)) {
                    if (fuel_[i] >= kMaxFuel) continue;
                    fuel_[i] = std::min(kMaxFuel, fuel_[i] + kFuelPickupAmount);
                } else {
                    if (ammo_[i] >= kMaxAmmo) continue;
                    ammo_[i] = std::uint16_t(std::min<int>(int(kMaxAmmo),
                                                           int(ammo_[i]) + int(kAmmoPickupAmount)));
                }
                tick_flags_[i] |= proto::kPlayerPick;
                field.take(k, tick, respawn_ticks);
            }
        }
    }

    // Returns true if this shot killed the victim.
    bool damage(std::uint32_t victim, std::uint16_t amount, std::uint64_t tick,
                std::uint64_t respawn_ticks) noexcept {
        tick_flags_[victim] |= proto::kPlayerHit;
        if (health_[victim] > amount) { health_[victim] = std::uint16_t(health_[victim] - amount); return false; }
        health_[victim]     = 0;
        alive_[victim]      = 0;
        respawn_at_[victim] = tick + respawn_ticks;
        tick_flags_[victim] |= proto::kPlayerDead;
        return true;
    }

    void add_score(std::uint32_t i) noexcept { ++score_[i]; }

    bool due_respawn(std::uint32_t i, std::uint64_t tick) const noexcept {
        return !alive_[i] && tick >= respawn_at_[i];
    }

private:
    std::vector<std::uint32_t> id_;
    std::vector<float>         x_, y_, z_, vy_, yaw_, pitch_;
    std::vector<std::uint16_t> health_, score_;
    std::vector<std::uint8_t>  alive_;
    std::vector<std::uint64_t> respawn_at_;
    std::vector<float>         cooldown_;
    std::vector<float>         fuel_;
    std::vector<std::uint16_t> ammo_;
    std::vector<float>         in_x_, in_z_;
    std::vector<std::uint32_t> in_btn_;
    std::vector<ClientLink>    link_;
    std::vector<std::uint32_t> last_seq_;
    std::vector<std::uint64_t> last_ts_;
    std::vector<std::uint16_t> tick_flags_;
    std::size_t                cap_ = 0;
};

} // namespace ge::world
