#pragma once
// Bump ("linear") arena over one up-front reservation.
//
// Per-tick scratch in the world simulation lives here: allocation is a pointer
// add, deallocation is a single reset() at the top of the next tick, and the
// backing memory is mapped once at startup. That is what makes the steady-state
// allocation count read exactly zero.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "ge/core/assert.hpp"
#include "ge/core/bits.hpp"
#include "ge/core/platform.hpp"

#include <sys/mman.h>

namespace ge::alloc {

class Arena {
public:
    Arena() = default;
    explicit Arena(std::size_t capacity) { reserve(capacity); }
    ~Arena() { release(); }

    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& o) noexcept
        : base_(o.base_), cap_(o.cap_), off_(o.off_), high_water_(o.high_water_) {
        o.base_ = nullptr; o.cap_ = o.off_ = o.high_water_ = 0;
    }
    Arena& operator=(Arena&& o) noexcept {
        if (this != &o) { release(); new (this) Arena(std::move(o)); }
        return *this;
    }

    // Anonymous mmap rather than malloc: one region, page-aligned, and we can
    // fault it in eagerly so the first tick does not pay for page faults.
    void reserve(std::size_t capacity) {
        release();
        cap_  = core::align_up(capacity, 4096);
        void* p = ::mmap(nullptr, cap_, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        GE_CHECK(p != MAP_FAILED);
        base_ = static_cast<std::byte*>(p);
        off_  = 0;
        high_water_ = 0;
    }

    // Touch every page so the measurement run never takes a minor fault.
    void prefault() noexcept {
        for (std::size_t i = 0; i < cap_; i += 4096) base_[i] = std::byte{0};
    }

    void release() noexcept {
        if (base_) { ::munmap(base_, cap_); base_ = nullptr; }
        cap_ = off_ = high_water_ = 0;
    }

    // Returns nullptr on exhaustion. Callers on the hot path treat that as
    // backpressure (drop this tick's payload and count it), never as a fatal.
    GE_ALWAYS_INLINE void* allocate(std::size_t n, std::size_t align = alignof(std::max_align_t)) noexcept {
        GE_ASSERT(core::is_pow2(align));
        const std::size_t start = core::align_up(off_, align);
        if (GE_UNLIKELY(start + n > cap_)) return nullptr;
        off_ = start + n;
        if (off_ > high_water_) high_water_ = off_;
        return base_ + start;
    }

    template <typename T>
    GE_ALWAYS_INLINE T* allocate_array(std::size_t count) noexcept {
        static_assert(std::is_trivially_destructible_v<T>,
                      "arena never runs destructors; store trivially destructible types only");
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    GE_ALWAYS_INLINE void reset() noexcept { off_ = 0; }

    // Scoped sub-allocation: release everything allocated after the mark.
    struct Mark { std::size_t off; };
    Mark mark() const noexcept { return {off_}; }
    void release_to(Mark m) noexcept { GE_ASSERT(m.off <= off_); off_ = m.off; }

    std::size_t used()       const noexcept { return off_; }
    std::size_t capacity()   const noexcept { return cap_; }
    std::size_t high_water() const noexcept { return high_water_; }  // sizes the reservation

private:
    std::byte*  base_       = nullptr;
    std::size_t cap_        = 0;
    std::size_t off_        = 0;
    std::size_t high_water_ = 0;
};

// RAII mark/release, for scratch that must not outlive a function.
class ArenaScope {
public:
    explicit ArenaScope(Arena& a) noexcept : a_(a), m_(a.mark()) {}
    ~ArenaScope() noexcept { a_.release_to(m_); }
    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;
private:
    Arena&       a_;
    Arena::Mark  m_;
};

} // namespace ge::alloc
