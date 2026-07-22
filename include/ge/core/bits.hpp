#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>

namespace ge::core {

constexpr bool is_pow2(std::size_t v) noexcept { return v && !(v & (v - 1)); }

constexpr std::size_t next_pow2(std::size_t v) noexcept {
    return v <= 1 ? 1 : std::size_t{1} << (64 - std::size_t(std::countl_zero(v - 1)));
}

constexpr std::size_t align_up(std::size_t v, std::size_t a) noexcept {
    return (v + a - 1) & ~(a - 1);
}

// FNV-1a: not cryptographic, but stable across processes, which is what the
// consistent-hash ring and the shard assignment need.
constexpr std::uint64_t fnv1a(const void* data, std::size_t n,
                              std::uint64_t seed = 1469598103934665603ULL) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

constexpr std::uint64_t mix64(std::uint64_t x) noexcept {   // splitmix64 finaliser
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// Deterministic PRNG: every benchmark run is reproducible from a seed.
class Xoshiro256ss {
public:
    explicit constexpr Xoshiro256ss(std::uint64_t seed) noexcept {
        for (auto& s : s_) { seed += 0x9e3779b97f4a7c15ULL; s = mix64(seed); }
    }
    constexpr std::uint64_t next() noexcept {
        const std::uint64_t r = std::rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
        s_[2] ^= t;     s_[3] = std::rotl(s_[3], 45);
        return r;
    }
    // Uniform in [0,1); 53 significant bits.
    constexpr double next_double() noexcept {
        return double(next() >> 11) * 0x1.0p-53;
    }
    constexpr std::uint32_t next_below(std::uint32_t n) noexcept {
        return std::uint32_t((next() >> 32) * n >> 32);   // Lemire, bias < 2^-32
    }
private:
    std::uint64_t s_[4]{};
};

} // namespace ge::core
