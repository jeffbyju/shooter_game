#pragma once
#include <cstdio>
#include <cstdlib>
#include "ge/core/platform.hpp"

namespace ge::core::detail {
[[noreturn]] GE_COLD GE_NOINLINE inline void
fail(const char* what, const char* expr, const char* file, int line) noexcept {
    std::fprintf(stderr, "\n[%s] %s\n  at %s:%d\n", what, expr, file, line);
    std::fflush(stderr);
    std::abort();
}
} // namespace ge::core::detail

// GE_CHECK is always on: use it for invariants whose violation means the
// process is already corrupt (bad fd, broken ring indices, protocol desync).
#define GE_CHECK(expr)                                                         \
    do {                                                                       \
        if (GE_UNLIKELY(!(expr)))                                              \
            ::ge::core::detail::fail("CHECK failed", #expr, __FILE__, __LINE__); \
    } while (0)

// GE_ASSERT compiles out in release; use it for hot-path preconditions.
#ifdef NDEBUG
  #define GE_ASSERT(expr) ((void)0)
#else
  #define GE_ASSERT(expr) GE_CHECK(expr)
#endif
