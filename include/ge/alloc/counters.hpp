#pragma once
// Process-wide allocation accounting.
//
// The claim "zero allocations on the hot path" is worthless unless it is
// measured, so the build interposes global operator new/delete and counts every
// call on a per-thread, cache-line-isolated counter block. The counters are
// plain non-atomic adds on the owning thread and are summed only when a
// snapshot is taken, so the probe costs ~1ns and does not perturb the thing it
// measures. Byte totals come from malloc_size()/malloc_usable_size(), i.e. the
// real footprint including allocator rounding, not the requested size.
#include <cstddef>
#include <cstdint>
#include "ge/core/platform.hpp"

namespace ge::alloc {

struct AllocStats {
    std::uint64_t allocs          = 0;
    std::uint64_t frees           = 0;
    std::uint64_t bytes_allocated = 0;
    std::uint64_t bytes_freed     = 0;

    std::int64_t live_allocs() const noexcept { return std::int64_t(allocs) - std::int64_t(frees); }
    std::int64_t live_bytes()  const noexcept { return std::int64_t(bytes_allocated) - std::int64_t(bytes_freed); }

    AllocStats operator-(const AllocStats& o) const noexcept {
        return {allocs - o.allocs, frees - o.frees,
                bytes_allocated - o.bytes_allocated, bytes_freed - o.bytes_freed};
    }
};

// Sum across every thread that has ever allocated. O(#threads), lock-free.
AllocStats snapshot() noexcept;

// True when the interposer is compiled in. When false, snapshot() returns
// zeroes and callers must report "not measured" rather than "zero".
bool probe_enabled() noexcept;

// Scopes a measurement region. delta() is the allocation activity inside it --
// the number that has to read 0 for a steady-state loop.
class AllocScope {
public:
    AllocScope() noexcept : base_(snapshot()) {}
    AllocStats delta() const noexcept { return snapshot() - base_; }
    void reset() noexcept { base_ = snapshot(); }
private:
    AllocStats base_;
};

} // namespace ge::alloc
