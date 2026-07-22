#pragma once
// Compiler / architecture facade. Everything platform-conditional in the code
// base funnels through here so the hot paths stay free of #ifdef noise.

#include <cstddef>
#include <cstdint>

#if defined(__APPLE__)
  #define GE_OS_MACOS 1
#elif defined(__linux__)
  #define GE_OS_LINUX 1
#else
  #error "unsupported platform"
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
  #define GE_ARCH_ARM64 1
#elif defined(__x86_64__)
  #define GE_ARCH_X86_64 1
#endif

#define GE_LIKELY(x)    __builtin_expect(!!(x), 1)
#define GE_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#define GE_ALWAYS_INLINE inline __attribute__((always_inline))
#define GE_NOINLINE      __attribute__((noinline))
#define GE_HOT           __attribute__((hot))
#define GE_COLD          __attribute__((cold))
// Promises the compiler that pointers do not alias. Note what this does and
// does not buy: clang will usually vectorise a unit-stride loop *without* it by
// emitting runtime alias checks plus a scalar fallback path. restrict removes
// those checks, which measurably matters only when the trip count is small
// enough that the check is not amortised (~45% at 4 elements, <1% at 4096).
#define GE_RESTRICT      __restrict__

namespace ge::core {

// Apple Silicon and modern x86 both prefetch in 128-byte units even though the
// L1 line is 64B; padding to 128 removes adjacent-line prefetcher false sharing.
inline constexpr std::size_t kCacheLine   = 64;
inline constexpr std::size_t kFalseShared = 128;

#define GE_CACHE_ALIGNED alignas(::ge::core::kFalseShared)

GE_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(GE_ARCH_ARM64)
    asm volatile("isb" ::: "memory");   // cheapest architectural spin hint on Apple cores
#elif defined(GE_ARCH_X86_64)
    asm volatile("pause" ::: "memory");
#endif
}

template <typename T>
GE_ALWAYS_INLINE void prefetch_read(const T* p) noexcept { __builtin_prefetch(p, 0, 3); }
template <typename T>
GE_ALWAYS_INLINE void prefetch_write(T* p) noexcept { __builtin_prefetch(p, 1, 3); }

// Stops the optimiser from deleting a computed value in benchmarks.
//
// The constraint differs per compiler on purpose. Clang accepts "r,m" and picks
// whichever is cheaper. GCC, given the same constraint, may choose a register
// and then warn that the value "may be used uninitialized" for any object it
// cannot prove is fully written -- a false positive that fires across every
// benchmark. Forcing the memory form on GCC sidesteps it and is equally
// effective: the value must exist in memory, so it cannot be optimised away.
template <typename T>
GE_ALWAYS_INLINE void do_not_optimize(T&& v) noexcept {
#if defined(__clang__)
    asm volatile("" : : "r,m"(v) : "memory");
#else
    asm volatile("" : : "m"(v) : "memory");
#endif
}
GE_ALWAYS_INLINE void compiler_barrier() noexcept { asm volatile("" ::: "memory"); }

} // namespace ge::core
