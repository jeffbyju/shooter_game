#pragma once
// Heightmap terrain, generated from a seed rather than transmitted.
//
// The design decision that matters is that the server never sends the terrain.
// Both sides run this same generator over the same seed and get the same
// lattice, so a 2000-unit world of hills costs *zero bytes on the wire*. That
// is not a micro-optimisation: the measured ceiling of this server is the
// instance's packet allowance, not its CPU (results/pps-cap/), so a bulk
// terrain transfer would spend the one budget that actually binds.
//
// The cost of that choice is that terrain is fixed at startup and identical
// for every client. Destructible or streamed terrain would need a real
// transfer path and a different design.
//
// Determinism rests on two things: the integer RNG in core/bits.hpp, which is
// specified bit-for-bit, and arithmetic limited to float +, -, * and compare.
// No transcendentals, no fused-multiply-add-dependent expressions, nothing
// whose result is allowed to differ between x86 and ARM. `terrain_matches` in
// tests/test_world.cpp asserts a fixed seed still produces a fixed digest, so
// a compiler that reassociates something will fail the build rather than
// desynchronise a client quietly.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ge/core/bits.hpp"

namespace ge::world {

// The lattice is sized from the world, not fixed, and the reason is measured.
// A fixed 129 points over the 2000-unit world this server actually runs gives
// 15.6 units per cell, so the narrowest feature is wider than the 64-unit view
// radius: a player sees a gentle tilt and never a hill. Worse, the steepest
// rise anywhere on such a map is 0.19 units per unit travelled, which is
// nothing -- so every slope is walkable and the step rule below is dead code.
//
// Targeting ~4 units per cell instead puts the finest features at roughly 16
// units across, which is cover a player can actually use at this view radius.
// Lattices are 2^k + 1 so every octave's grid lands exactly on lattice points
// and no octave needs an edge case; 513 caps it at 1 MB of float, generated
// once at startup and then read-only.
inline constexpr float kTerrainCellTarget = 4.0f;
inline constexpr int   kTerrainLatticeMin = 33;
inline constexpr int   kTerrainLatticeMax = 513;
// The steepest ground a player may walk up, as rise over run. Dimensionless on
// purpose, and the first version of this was not: it compared the rise against
// an absolute 2.5 units, which sounds reasonable until you notice that one tick
// of walking covers kMoveSpeed/60 = 0.15 units. A 2.5-unit rise inside 0.15
// units of travel is a gradient of 16.7, i.e. a wall at 87 degrees. The rule
// was unreachable by construction and the test that proved it only failed
// because the test looked for a cliff the generator cannot make.
//
// 1.0 is 45 degrees. At the amplitudes this server runs the steepest ground
// anywhere is 0.29, so nothing is blocked and this costs only the comparison --
// it exists for small maps with tall terrain, where gradients reach 4.9.
inline constexpr float kTerrainMaxSlope = 1.0f;

class Terrain {
public:
    // amplitude is the height of the tallest feature before normalisation.
    void configure(float world_size, std::uint64_t seed, float amplitude) {
        world_ = world_size;

        // Smallest 2^k+1 lattice whose cell is no coarser than the target.
        n_ = kTerrainLatticeMin;
        while (n_ < kTerrainLatticeMax &&
               world_size / float(n_ - 1) > kTerrainCellTarget) {
            n_ = (n_ - 1) * 2 + 1;
        }
        inv_step_ = float(n_ - 1) / world_size;
        h_.assign(std::size_t(n_) * std::size_t(n_), 0.0f);

        // Octave count follows the lattice so the finest octave always spans
        // about four lattice cells. Fixing it instead would mean the finest
        // detail got coarser every time the map got bigger.
        int octaves = 0;
        for (int m = n_ - 1; m > 4; m >>= 1) ++octaves;

        core::Xoshiro256ss rng(seed);
        float amp = amplitude;

        // Fractal sum: each octave has twice the feature density and half the
        // amplitude of the one before. Generated as a small random grid that is
        // then interpolated up, rather than by evaluating a noise function per
        // lattice point -- the upsample is the same arithmetic every time,
        // which is what keeps it reproducible.
        for (int oct = 0; oct < octaves; ++oct) {
            const int g = (1 << (oct + 1)) + 1;          // 3, 5, 9, 17, 33
            std::vector<float> corner(std::size_t(g) * std::size_t(g));
            for (float& c : corner) c = float(rng.next_double()) * 2.0f - 1.0f;

            const float scale = float(g - 1) / float(n_ - 1);
            for (int r = 0; r < n_; ++r) {
                const float gz = float(r) * scale;
                const int   r0 = int(gz);
                const int   r1 = std::min(r0 + 1, g - 1);
                const float tz = gz - float(r0);
                // Smoothstep between octave corners. Plain bilinear here leaves
                // visible creases along every octave's grid lines, which read as
                // a tiled floor rather than as ground.
                const float sz = tz * tz * (3.0f - 2.0f * tz);
                for (int c = 0; c < n_; ++c) {
                    const float gx = float(c) * scale;
                    const int   c0 = int(gx);
                    const int   c1 = std::min(c0 + 1, g - 1);
                    const float tx = gx - float(c0);
                    const float sx = tx * tx * (3.0f - 2.0f * tx);

                    const std::size_t gz0 = std::size_t(r0) * std::size_t(g);
                    const std::size_t gz1 = std::size_t(r1) * std::size_t(g);
                    const float a  = corner[gz0 + std::size_t(c0)];
                    const float b  = corner[gz0 + std::size_t(c1)];
                    const float cc = corner[gz1 + std::size_t(c0)];
                    const float d  = corner[gz1 + std::size_t(c1)];
                    const float top = a + (b - a) * sx;
                    const float bot = cc + (d - cc) * sx;
                    h_[std::size_t(r) * std::size_t(n_) + std::size_t(c)] +=
                        (top + (bot - top) * sz) * amp;
                }
            }
            amp *= 0.5f;
        }

        // Shift so the lowest point sits at y = 0. Without it roughly half the
        // map is below the old floor plane, which puts spawns and pickups
        // underground and makes every existing y-based assertion wrong.
        float lo = h_.empty() ? 0.0f : h_[0];
        for (float v : h_) lo = std::min(lo, v);
        max_ = 0.0f;
        for (float& v : h_) { v -= lo; max_ = std::max(max_, v); }
    }

    bool  enabled()    const noexcept { return !h_.empty(); }
    float max_height() const noexcept { return max_; }
    int   lattice()    const noexcept { return n_; }
    std::size_t bytes() const noexcept { return h_.capacity() * sizeof(float); }

    // Ground height under a world position. Bilinear, so the surface is
    // continuous but not smooth -- there is a crease along every lattice line.
    // That is deliberate: a bicubic lookup costs 16 samples instead of 4 and
    // the crease is invisible at 15 units per cell. If it ever shows, the fix
    // is a denser lattice, not a fancier filter.
    float height(float x, float z) const noexcept {
        if (h_.empty()) return 0.0f;
        const float fx = std::clamp(x * inv_step_, 0.0f, float(n_ - 1));
        const float fz = std::clamp(z * inv_step_, 0.0f, float(n_ - 1));
        const int   c0 = int(fx), r0 = int(fz);
        const int   c1 = std::min(c0 + 1, n_ - 1);
        const int   r1 = std::min(r0 + 1, n_ - 1);
        const float tx = fx - float(c0), tz = fz - float(r0);

        const float a = h_[std::size_t(r0) * std::size_t(n_) + std::size_t(c0)];
        const float b = h_[std::size_t(r0) * std::size_t(n_) + std::size_t(c1)];
        const float c = h_[std::size_t(r1) * std::size_t(n_) + std::size_t(c0)];
        const float d = h_[std::size_t(r1) * std::size_t(n_) + std::size_t(c1)];
        const float top = a + (b - a) * tx;
        const float bot = c + (d - c) * tx;
        return top + (bot - top) * tz;
    }

    // A digest of the whole lattice, for asserting that two builds agree.
    // Integer arithmetic over the float bit patterns, so it catches a value
    // that differs in the last mantissa bit -- which is exactly the kind of
    // divergence that would desynchronise a client and never show up in a
    // screenshot.
    std::uint64_t digest() const noexcept {
        std::uint64_t h = 1469598103934665603ull;          // FNV-1a offset
        for (float v : h_) {
            std::uint32_t bits = 0;
            __builtin_memcpy(&bits, &v, sizeof(bits));
            for (int b = 0; b < 4; ++b) {
                h ^= std::uint64_t((bits >> (b * 8)) & 0xFF);
                h *= 1099511628211ull;
            }
        }
        return h;
    }

private:
    int    n_        = 0;
    float  world_    = 0.0f;
    float  inv_step_ = 0.0f;
    float  max_      = 0.0f;
    std::vector<float> h_;
};

}  // namespace ge::world
