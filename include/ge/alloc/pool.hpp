#pragma once
// Fixed-size object pool: one contiguous slab, an intrusive free list threaded
// through the free slots themselves, so acquire/release are a load and a store
// with no metadata cache line of their own.
//
// When to use it, and when not to.
//
// It is the right tool for objects whose lifetime is confined to one thread:
// acquire and release are a load and a store, the slab is contiguous, and
// capacity is bounded at startup so admission control is a compare rather than
// an allocation failure deep inside a handler.
//
// It is the wrong tool when an index into it crosses a thread boundary and may
// outlive the object. The free-list link lives *inside* the freed slot, so a
// released object's first 8 bytes become a pointer; another thread still
// holding that index reads a pointer where it expects the object's first
// fields. The world server hit exactly this -- a simulation shard holds a
// connection index while a state delta is in flight, and `Connection` keeps its
// file descriptor in the first 8 bytes, so a recycled slot read back as a
// plausible fd. Connections therefore live in a fixed slab with a separate
// index free list, which keeps every slot readable forever. See
// docs/ARCHITECTURE.md.
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "ge/alloc/arena.hpp"
#include "ge/core/assert.hpp"

namespace ge::alloc {

template <typename T>
class Pool {
    static_assert(sizeof(T) >= sizeof(void*), "slot must be able to hold a free-list link");
public:
    Pool() = default;
    explicit Pool(std::size_t capacity) { reserve(capacity); }
    ~Pool() { clear(); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    void reserve(std::size_t capacity) {
        clear();
        arena_.reserve(capacity * sizeof(Slot) + 4096);
        arena_.prefault();
        slots_ = arena_.allocate_array<Slot>(capacity);
        GE_CHECK(slots_ != nullptr);
        cap_ = capacity;
        // Thread the free list in reverse so the first acquire hands out slot 0
        // and the hot working set stays at the front of the slab.
        free_ = nullptr;
        for (std::size_t i = capacity; i-- > 0;) {
            slots_[i].next = free_;
            free_ = &slots_[i];
        }
        live_ = 0;
    }

    template <typename... Args>
    T* acquire(Args&&... args) noexcept {
        if (GE_UNLIKELY(free_ == nullptr)) return nullptr;   // at capacity: caller applies admission control
        Slot* s = free_;
        free_ = s->next;
        ++live_;
        if (live_ > high_water_) high_water_ = live_;
        return ::new (static_cast<void*>(s->storage)) T(std::forward<Args>(args)...);
    }

    void release(T* p) noexcept {
        if (!p) return;
        GE_ASSERT(owns(p));
        p->~T();
        Slot* s = reinterpret_cast<Slot*>(p);
        s->next = free_;
        free_ = s;
        --live_;
    }

    bool owns(const T* p) const noexcept {
        const auto* q = reinterpret_cast<const Slot*>(p);
        return q >= slots_ && q < slots_ + cap_;
    }
    // Stable dense index: the wire handle for a connection, no pointer chasing.
    std::uint32_t index_of(const T* p) const noexcept {
        GE_ASSERT(owns(p));
        return std::uint32_t(reinterpret_cast<const Slot*>(p) - slots_);
    }
    T* at(std::uint32_t i) noexcept {
        GE_ASSERT(i < cap_);
        return reinterpret_cast<T*>(slots_[i].storage);
    }

    std::size_t live()       const noexcept { return live_; }
    std::size_t capacity()   const noexcept { return cap_; }
    std::size_t high_water() const noexcept { return high_water_; }
    std::size_t bytes()      const noexcept { return cap_ * sizeof(Slot); }

    void clear() noexcept { slots_ = nullptr; free_ = nullptr; cap_ = live_ = 0; arena_.release(); }

private:
    // Trivial union: the free-list link lives in the slot's own storage, so a
    // free slot costs no side table and an occupied one costs no header.
    union Slot {
        Slot*      next;
        alignas(T) std::byte storage[sizeof(T)];
    };
    static_assert(std::is_trivially_destructible_v<Slot>);

    Arena       arena_;
    Slot*       slots_      = nullptr;
    Slot*       free_       = nullptr;
    std::size_t cap_        = 0;
    std::size_t live_       = 0;
    std::size_t high_water_ = 0;
};

} // namespace ge::alloc
