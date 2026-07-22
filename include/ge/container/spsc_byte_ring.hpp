#pragma once
// SPSC ring of variable-length records, with the guarantee that every record is
// physically contiguous.
//
// That guarantee is the whole point: the producer (a world shard) serialises a
// packet directly into ring memory and the consumer (an I/O thread) hands the
// same bytes to write(2). The payload is never copied between the two -- not
// into a std::vector, not into a per-connection staging buffer.
//
// Contiguity is kept by emitting a pad record when a payload would straddle the
// end of the buffer, which costs at most one wasted tail fragment per wrap.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "ge/alloc/arena.hpp"
#include "ge/core/assert.hpp"
#include "ge/core/bits.hpp"
#include "ge/core/platform.hpp"

namespace ge::container {

class SpscByteRing {
public:
    static constexpr std::uint32_t kPad      = 0xFFFF'FFFFu;  // "skip to buffer start"
    static constexpr std::size_t   kHeader   = 8;             // len + 4B pad, keeps payloads 8-aligned
    static constexpr std::size_t   kMaxRecord= 1u << 20;

    SpscByteRing() = default;
    explicit SpscByteRing(std::size_t capacity) { reserve(capacity); }

    SpscByteRing(const SpscByteRing&)            = delete;
    SpscByteRing& operator=(const SpscByteRing&) = delete;

    void reserve(std::size_t capacity) {
        cap_  = core::next_pow2(capacity);
        mask_ = cap_ - 1;
        arena_.reserve(cap_ + 4096);
        arena_.prefault();
        buf_ = arena_.allocate_array<std::byte>(cap_);
        GE_CHECK(buf_ != nullptr);
    }

    // ---- producer: reserve, fill in place, commit ---------------------------
    // Returns a contiguous, 8-byte-aligned span of at least `max_bytes`, or an
    // empty span when the ring is full. Nothing is published until commit().
    GE_ALWAYS_INLINE std::byte* begin_write(std::uint32_t max_bytes) noexcept {
        GE_ASSERT(max_bytes <= kMaxRecord);
        const std::size_t need = kHeader + core::align_up(max_bytes, 8);
        const std::uint64_t head = head_.pos.load(std::memory_order_relaxed);
        std::size_t off = std::size_t(head & mask_);

        if (off + need > cap_) {                       // would straddle: pad to the end
            const std::size_t pad = cap_ - off;
            if (!has_room(head, pad + need)) return nullptr;
            store_len(off, kPad);
            head_.pos.store(head + pad, std::memory_order_release);
            pending_head_ = head + pad;
            off = 0;
        } else {
            if (!has_room(head, need)) return nullptr;
            pending_head_ = head;
        }
        pending_off_ = off;
        return buf_ + off + kHeader;
    }

    GE_ALWAYS_INLINE void commit_write(std::uint32_t bytes) noexcept {
        GE_ASSERT(bytes != kPad);
        store_len(pending_off_, bytes);
        const std::size_t total = kHeader + core::align_up(bytes, 8);
        head_.pos.store(pending_head_ + total, std::memory_order_release);
    }

    // Convenience wrapper for callers that already hold the bytes.
    bool try_write(const void* data, std::uint32_t n) noexcept {
        std::byte* dst = begin_write(n);
        if (!dst) return false;
        std::memcpy(dst, data, n);
        commit_write(n);
        return true;
    }

    // ---- consumer: peek the contiguous payload, then pop -------------------
    GE_ALWAYS_INLINE std::span<const std::byte> peek() noexcept {
        std::uint64_t tail = tail_.pos.load(std::memory_order_relaxed);
        for (;;) {
            if (tail == cached_head_) {
                cached_head_ = head_.pos.load(std::memory_order_acquire);
                if (tail == cached_head_) { peek_tail_ = tail; peek_len_ = 0; return {}; }
            }
            const std::size_t off = std::size_t(tail & mask_);
            const std::uint32_t len = load_len(off);
            if (len == kPad) {                          // consume the pad, retry at 0
                tail += (cap_ - off);
                tail_.pos.store(tail, std::memory_order_release);
                continue;
            }
            peek_tail_ = tail;
            peek_len_  = len;
            return {buf_ + off + kHeader, len};
        }
    }

    GE_ALWAYS_INLINE void pop() noexcept {
        const std::size_t total = kHeader + core::align_up(peek_len_, 8);
        tail_.pos.store(peek_tail_ + total, std::memory_order_release);
    }

    std::size_t used() const noexcept {
        return std::size_t(head_.pos.load(std::memory_order_acquire) -
                           tail_.pos.load(std::memory_order_acquire));
    }
    std::size_t capacity() const noexcept { return cap_; }
    bool        empty()    const noexcept { return used() == 0; }

private:
    GE_ALWAYS_INLINE bool has_room(std::uint64_t head, std::size_t need) noexcept {
        if (cap_ - std::size_t(head - cached_tail_) >= need) return true;
        cached_tail_ = tail_.pos.load(std::memory_order_acquire);
        return cap_ - std::size_t(head - cached_tail_) >= need;
    }
    GE_ALWAYS_INLINE void store_len(std::size_t off, std::uint32_t v) noexcept {
        std::memcpy(buf_ + off, &v, sizeof v);
    }
    GE_ALWAYS_INLINE std::uint32_t load_len(std::size_t off) const noexcept {
        std::uint32_t v; std::memcpy(&v, buf_ + off, sizeof v); return v;
    }

    struct GE_CACHE_ALIGNED Index { std::atomic<std::uint64_t> pos{0}; };

    alloc::Arena  arena_;
    std::byte*    buf_  = nullptr;
    std::size_t   cap_  = 0;
    std::uint64_t mask_ = 0;

    Index head_;
    GE_CACHE_ALIGNED std::uint64_t cached_tail_  = 0;   // producer-private
    std::uint64_t                  pending_head_ = 0;
    std::size_t                    pending_off_  = 0;

    Index tail_;
    GE_CACHE_ALIGNED std::uint64_t cached_head_ = 0;    // consumer-private
    std::uint64_t                  peek_tail_   = 0;
    std::uint32_t                  peek_len_    = 0;
};

} // namespace ge::container
