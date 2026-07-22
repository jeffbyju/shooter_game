#pragma once
// Single-producer / single-consumer bounded ring of trivially copyable T.
//
// The two indices sit on separate 128-byte lines and each side keeps a private
// cached copy of the other's index, so the common case (ring neither full nor
// empty) touches only the producer's own line: one acquire load amortised over
// many pushes instead of a coherence miss per push.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ge/alloc/arena.hpp"
#include "ge/core/assert.hpp"
#include "ge/core/bits.hpp"
#include "ge/core/platform.hpp"

namespace ge::container {

template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SPSC slots are raw-copied");
public:
    SpscRing() = default;
    explicit SpscRing(std::size_t capacity) { reserve(capacity); }

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Capacity is rounded up to a power of two so the index wrap is a mask.
    void reserve(std::size_t capacity) {
        cap_  = core::next_pow2(capacity);
        mask_ = cap_ - 1;
        arena_.reserve(cap_ * sizeof(T) + 4096);
        arena_.prefault();                       // no page faults during measurement
        slots_ = arena_.allocate_array<T>(cap_);
        GE_CHECK(slots_ != nullptr);
    }

    // ---- producer side -----------------------------------------------------
    GE_ALWAYS_INLINE bool try_push(const T& v) noexcept {
        const std::uint64_t head = head_.pos.load(std::memory_order_relaxed);
        if (GE_UNLIKELY(head - cached_tail_ >= cap_)) {
            cached_tail_ = tail_.pos.load(std::memory_order_acquire);
            if (head - cached_tail_ >= cap_) return false;      // full: caller decides drop vs retry
        }
        // cap_ is a power of two and mask_ == cap_ - 1, established in reserve().
        // Stating it lets the compiler drop the bounds reasoning entirely.
        [[assume((mask_ & (mask_ + 1)) == 0)]];
        slots_[head & mask_] = v;
        head_.pos.store(head + 1, std::memory_order_release);
        return true;
    }

    // ---- consumer side -----------------------------------------------------
    GE_ALWAYS_INLINE bool try_pop(T& out) noexcept {
        const std::uint64_t tail = tail_.pos.load(std::memory_order_relaxed);
        if (GE_UNLIKELY(tail == cached_head_)) {
            cached_head_ = head_.pos.load(std::memory_order_acquire);
            if (tail == cached_head_) return false;             // empty
        }
        out = slots_[tail & mask_];
        tail_.pos.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Drain in one shot: one acquire load and one release store for the whole
    // batch instead of per item. This is the call the shard threads use.
    template <typename Fn>
    std::size_t drain(Fn&& fn, std::size_t max = static_cast<std::size_t>(-1)) noexcept {
        const std::uint64_t tail = tail_.pos.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.pos.load(std::memory_order_acquire);
        std::size_t n = std::size_t(head - tail);
        if (n == 0) return 0;
        if (n > max) n = max;
        for (std::size_t i = 0; i < n; ++i) {
            core::prefetch_read(&slots_[(tail + i + 4) & mask_]);
            fn(slots_[(tail + i) & mask_]);
        }
        tail_.pos.store(tail + n, std::memory_order_release);
        return n;
    }

    std::size_t size() const noexcept {
        return std::size_t(head_.pos.load(std::memory_order_acquire) -
                           tail_.pos.load(std::memory_order_acquire));
    }
    bool        empty()    const noexcept { return size() == 0; }
    std::size_t capacity() const noexcept { return cap_; }

private:
    struct GE_CACHE_ALIGNED Index { std::atomic<std::uint64_t> pos{0}; };

    alloc::Arena  arena_;
    T*            slots_ = nullptr;
    std::size_t   cap_   = 0;
    std::uint64_t mask_  = 0;

    Index head_;                                     // written by producer
    GE_CACHE_ALIGNED std::uint64_t cached_tail_ = 0; // producer-private
    Index tail_;                                     // written by consumer
    GE_CACHE_ALIGNED std::uint64_t cached_head_ = 0; // consumer-private
};

} // namespace ge::container
