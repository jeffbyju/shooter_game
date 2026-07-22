#pragma once
// Bounded MPMC queue (Vyukov). Lock-free, wait-free for a single producer, and
// -- the property that matters here -- it never allocates after construction,
// so an enqueue from the accept path cannot stall on the malloc lock.
//
// Used for acceptor -> I/O-thread handoff and for control-plane traffic. The
// steady-state data path uses SpscRing instead: single-producer is strictly
// cheaper and the topology is known at startup.
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
class MpmcQueue {
    static_assert(std::is_trivially_copyable_v<T>);
public:
    MpmcQueue() = default;
    explicit MpmcQueue(std::size_t capacity) { reserve(capacity); }

    MpmcQueue(const MpmcQueue&)            = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    void reserve(std::size_t capacity) {
        cap_  = core::next_pow2(capacity);
        mask_ = cap_ - 1;
        arena_.reserve(cap_ * sizeof(Cell) + 4096);
        arena_.prefault();
        cells_ = arena_.allocate_array<Cell>(cap_);
        GE_CHECK(cells_ != nullptr);
        for (std::size_t i = 0; i < cap_; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
        head_.pos.store(0, std::memory_order_relaxed);
        tail_.pos.store(0, std::memory_order_relaxed);
    }

    bool try_push(const T& v) noexcept {
        std::uint64_t pos = head_.pos.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const std::uint64_t seq = c.seq.load(std::memory_order_acquire);
            const std::int64_t  d   = std::int64_t(seq) - std::int64_t(pos);
            if (d == 0) {
                if (head_.pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    c.value = v;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (d < 0) {
                return false;                                  // full
            } else {
                pos = head_.pos.load(std::memory_order_relaxed); // lost the race, re-read
            }
        }
    }

    bool try_pop(T& out) noexcept {
        std::uint64_t pos = tail_.pos.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const std::uint64_t seq = c.seq.load(std::memory_order_acquire);
            const std::int64_t  d   = std::int64_t(seq) - std::int64_t(pos + 1);
            if (d == 0) {
                if (tail_.pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = c.value;
                    c.seq.store(pos + mask_ + 1, std::memory_order_release);
                    return true;
                }
            } else if (d < 0) {
                return false;                                  // empty
            } else {
                pos = tail_.pos.load(std::memory_order_relaxed);
            }
        }
    }

    std::size_t size_approx() const noexcept {
        const std::uint64_t h = head_.pos.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.pos.load(std::memory_order_relaxed);
        return h > t ? std::size_t(h - t) : 0;
    }
    std::size_t capacity() const noexcept { return cap_; }

private:
    struct Cell {
        std::atomic<std::uint64_t> seq;
        T                          value;
    };
    struct GE_CACHE_ALIGNED Index { std::atomic<std::uint64_t> pos{0}; };

    alloc::Arena  arena_;
    Cell*         cells_ = nullptr;
    std::size_t   cap_   = 0;
    std::uint64_t mask_  = 0;
    Index         head_;
    Index         tail_;
};

} // namespace ge::container
