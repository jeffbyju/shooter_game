#include "ge/alloc/counters.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(GE_OS_MACOS)
  #include <malloc/malloc.h>
  #define GE_USABLE_SIZE(p) ::malloc_size(p)
#elif defined(GE_OS_LINUX)
  #include <malloc.h>
  #define GE_USABLE_SIZE(p) ::malloc_usable_size(p)
#else
  #define GE_USABLE_SIZE(p) (std::size_t{0})
#endif

namespace ge::alloc {
namespace {

constexpr int kMaxThreads = 512;

constexpr std::size_t kFalseShare = core::kFalseShared;

// One counter block per thread, padded so two threads never share a line.
struct alignas(kFalseShare) ThreadCounters {
    std::uint64_t allocs;
    std::uint64_t frees;
    std::uint64_t bytes_allocated;
    std::uint64_t bytes_freed;
};

// The counter blocks are a static array, not thread_local objects.
//
// An over-aligned thread_local is not portably honoured: Darwin's TLS allocator
// ignores alignas beyond pointer size, so `alignas(128) thread_local` silently
// yields a block that is *not* 128-byte aligned -- which defeats the entire
// purpose of the padding, because two threads' counters can then land on one
// cache line and contend on every increment. (UBSan's misaligned-reference
// check catches this; it is not a theoretical concern.) A static array of an
// over-aligned type is aligned for real, so the blocks are kept here and the
// thread-local holds only an index into them.
alignas(kFalseShare) ThreadCounters g_counters[kMaxThreads];
std::atomic<int>                    g_slot_count{0};

// Constant-initialised, so no dynamic initialiser runs and touching this from
// inside operator new cannot re-enter operator new.
thread_local int t_slot = -1;

// Overflow sink for the (unexpected) case of more than kMaxThreads threads:
// the counts are still correct in aggregate, they just share a line.
ThreadCounters g_overflow{};

GE_ALWAYS_INLINE ThreadCounters& counters() noexcept {
    if (GE_UNLIKELY(t_slot < 0)) {
        const int idx = g_slot_count.fetch_add(1, std::memory_order_acq_rel);
        t_slot = idx < kMaxThreads ? idx : kMaxThreads;
    }
    return t_slot < kMaxThreads ? g_counters[t_slot] : g_overflow;
}

} // namespace

void* track_alloc(void* p) noexcept {
    if (GE_LIKELY(p != nullptr)) {
        ThreadCounters& c = counters();
        ++c.allocs;
        c.bytes_allocated += GE_USABLE_SIZE(p);
    }
    return p;
}

void track_free(void* p) noexcept {
    if (GE_LIKELY(p != nullptr)) {
        ThreadCounters& c = counters();
        ++c.frees;
        c.bytes_freed += GE_USABLE_SIZE(p);
    }
}

AllocStats snapshot() noexcept {
    AllocStats out;
#if GE_ALLOC_PROBE
    int n = g_slot_count.load(std::memory_order_acquire);
    if (n > kMaxThreads) n = kMaxThreads;
    for (int i = 0; i < n; ++i) {
        // Plain reads of another thread's counters: a summary may be skewed by
        // one in-flight operation, which is irrelevant at the scale these are
        // reported at and keeps the hot path free of atomics.
        const ThreadCounters& c = g_counters[i];
        out.allocs          += c.allocs;
        out.frees           += c.frees;
        out.bytes_allocated += c.bytes_allocated;
        out.bytes_freed     += c.bytes_freed;
    }
    out.allocs          += g_overflow.allocs;
    out.frees           += g_overflow.frees;
    out.bytes_allocated += g_overflow.bytes_allocated;
    out.bytes_freed     += g_overflow.bytes_freed;
#endif
    return out;
}

bool probe_enabled() noexcept {
#if GE_ALLOC_PROBE
    return true;
#else
    return false;
#endif
}

} // namespace ge::alloc

// ---------------------------------------------------------------------------
// Global operator new/delete interposition. Every overload libc++ can route
// through is covered, including sized and over-aligned forms -- missing one
// would silently under-count.
// ---------------------------------------------------------------------------
#if GE_ALLOC_PROBE

namespace ge::alloc {
void* track_alloc(void*) noexcept;
void  track_free(void*) noexcept;
} // namespace ge::alloc

namespace {

[[noreturn]] GE_COLD void oom() {
#if __cpp_exceptions
    throw std::bad_alloc();
#else
    std::abort();
#endif
}
inline void* checked(void* p) { if (GE_UNLIKELY(!p)) oom(); return p; }

inline void* aligned(std::size_t n, std::align_val_t a) noexcept {
    const std::size_t al = std::size_t(a);
    const std::size_t sz = n ? ((n + al - 1) / al) * al : al;   // aligned_alloc needs size % align == 0
    return ge::alloc::track_alloc(std::aligned_alloc(al, sz));
}

} // namespace

void* operator new(std::size_t n)                                   { return checked(ge::alloc::track_alloc(std::malloc(n ? n : 1))); }
void* operator new[](std::size_t n)                                 { return checked(ge::alloc::track_alloc(std::malloc(n ? n : 1))); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept   { return ge::alloc::track_alloc(std::malloc(n ? n : 1)); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ge::alloc::track_alloc(std::malloc(n ? n : 1)); }

void* operator new(std::size_t n, std::align_val_t a)                                 { return checked(aligned(n, a)); }
void* operator new[](std::size_t n, std::align_val_t a)                               { return checked(aligned(n, a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return aligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return aligned(n, a); }

void operator delete(void* p) noexcept                                             { ge::alloc::track_free(p); std::free(p); }
void operator delete[](void* p) noexcept                                           { ge::alloc::track_free(p); std::free(p); }
void operator delete(void* p, std::size_t) noexcept                                { ge::alloc::track_free(p); std::free(p); }
void operator delete[](void* p, std::size_t) noexcept                              { ge::alloc::track_free(p); std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept                      { ge::alloc::track_free(p); std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept                    { ge::alloc::track_free(p); std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept                           { ge::alloc::track_free(p); std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept                         { ge::alloc::track_free(p); std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept              { ge::alloc::track_free(p); std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept            { ge::alloc::track_free(p); std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept    { ge::alloc::track_free(p); std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept  { ge::alloc::track_free(p); std::free(p); }

#endif // GE_ALLOC_PROBE
