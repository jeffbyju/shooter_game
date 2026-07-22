#pragma once
// Generational slot map: O(1) insert/erase/lookup over a *dense* value array.
//
// Two properties earn its place on the hot path:
//   1. Iteration is a linear scan of packed values -- the per-tick simulation
//      walks entities with a perfect stride and no pointer chasing.
//   2. Handles carry a generation, so a stale handle held by another thread
//      (an entity that disconnected two ticks ago) resolves to null instead of
//      silently aliasing whoever reused the slot. That is a use-after-free that
//      a raw index would not catch.
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "ge/core/assert.hpp"
#include "ge/core/platform.hpp"

namespace ge::container {

struct Handle {
    std::uint32_t index = 0;
    std::uint32_t gen   = 0;                     // 0 == never issued

    constexpr bool valid() const noexcept { return gen != 0; }
    constexpr bool operator==(const Handle&) const noexcept = default;
    constexpr std::uint64_t bits() const noexcept {
        return (std::uint64_t(gen) << 32) | index; // wire-safe packed form
    }
    static constexpr Handle from_bits(std::uint64_t b) noexcept {
        return {std::uint32_t(b & 0xFFFF'FFFFu), std::uint32_t(b >> 32)};
    }
};

template <typename T>
class SlotMap {
public:
    explicit SlotMap(std::size_t capacity = 0) { reserve(capacity); }

    // All storage is reserved up front; steady state performs no reallocation.
    void reserve(std::size_t n) {
        values_.reserve(n);
        value_to_slot_.reserve(n);
        slots_.reserve(n);
    }

    Handle insert(const T& v) {
        std::uint32_t slot;
        if (free_head_ != kNoFree) {
            slot = free_head_;
            free_head_ = slots_[slot].next_free;
        } else {
            slot = std::uint32_t(slots_.size());
            slots_.push_back({0, 1, kNoFree});
        }
        Slot& s = slots_[slot];
        if (s.gen == 0) s.gen = 1;               // gen 0 is reserved for "invalid"
        s.next_free = kNoFree;                   // mark occupied
        s.dense     = std::uint32_t(values_.size());
        values_.push_back(v);
        value_to_slot_.push_back(slot);
        ++live_;
        return {slot, s.gen};
    }

    bool erase(Handle h) noexcept {
        T* p = find(h);
        if (!p) return false;
        Slot& s = slots_[h.index];
        const std::uint32_t dense = s.dense;
        const std::uint32_t last  = std::uint32_t(values_.size() - 1);
        if (dense != last) {                     // swap-and-pop keeps values_ dense
            values_[dense]        = values_[last];
            value_to_slot_[dense] = value_to_slot_[last];
            slots_[value_to_slot_[dense]].dense = dense;
        }
        values_.pop_back();
        value_to_slot_.pop_back();
        if (++s.gen == 0) s.gen = 1;             // bump so old handles stop resolving
        s.next_free = free_head_;
        free_head_  = h.index;
        --live_;
        return true;
    }

    GE_ALWAYS_INLINE T* find(Handle h) noexcept {
        if (GE_UNLIKELY(!h.valid() || h.index >= slots_.size())) return nullptr;
        Slot& s = slots_[h.index];
        if (GE_UNLIKELY(s.gen != h.gen || s.next_free != kNoFree)) return nullptr;
        return &values_[s.dense];
    }
    GE_ALWAYS_INLINE const T* find(Handle h) const noexcept {
        return const_cast<SlotMap*>(this)->find(h);
    }

    // Dense views for the simulation loop.
    T*       data()       noexcept { return values_.data(); }
    const T* data() const noexcept { return values_.data(); }
    std::size_t size()  const noexcept { return values_.size(); }
    bool        empty() const noexcept { return values_.empty(); }
    auto begin() noexcept { return values_.begin(); }
    auto end()   noexcept { return values_.end(); }
    auto begin() const noexcept { return values_.begin(); }
    auto end()   const noexcept { return values_.end(); }

    Handle handle_at(std::size_t dense) const noexcept {
        const std::uint32_t slot = value_to_slot_[dense];
        return {slot, slots_[slot].gen};
    }
    std::size_t live() const noexcept { return live_; }
    std::size_t bytes() const noexcept {
        return values_.capacity() * sizeof(T)
             + value_to_slot_.capacity() * sizeof(std::uint32_t)
             + slots_.capacity() * sizeof(Slot);
    }

private:
    static constexpr std::uint32_t kNoFree = 0xFFFF'FFFFu;
    struct Slot {
        std::uint32_t dense;
        std::uint32_t gen;
        std::uint32_t next_free;                 // kNoFree when occupied
    };

    std::vector<T>             values_;
    std::vector<std::uint32_t> value_to_slot_;
    std::vector<Slot>          slots_;
    std::uint32_t              free_head_ = kNoFree;
    std::size_t                live_      = 0;
};

} // namespace ge::container
