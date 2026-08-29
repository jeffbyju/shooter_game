#pragma once
// Monotonic timestamping for latency measurement.
//
// Measurement instrument matters as much as the thing measured, so this header
// also exposes `Clock::probe()`, which reports the clock's own call overhead and
// its observable granularity. Every latency number in the benchmark output is quoted alongside
// those two figures; a p50 near the granularity floor is not a measurement.
#include <cstdint>
#include <ctime>
#include "ge/core/assert.hpp"
#include "ge/core/platform.hpp"

#if defined(GE_OS_MACOS)
  #include <time.h>
#endif

namespace ge::core {

struct ClockProbe {
    std::int64_t overhead_ns;     // median cost of one now_ns() call
    std::int64_t granularity_ns;  // smallest non-zero delta observable
};

class Clock {
public:
    // CLOCK_UPTIME_RAW on Darwin and CLOCK_MONOTONIC on Linux are both served
    // from the commpage/vDSO: no syscall, ~15-25ns, and immune to NTP slew.
    static GE_ALWAYS_INLINE std::int64_t now_ns() noexcept {
#if defined(GE_OS_MACOS)
        return std::int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#else
        timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return std::int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
    }

    // Per-thread CPU time; the difference between this and wall time is how
    // much of a thread's life was spent off-core (blocked, descheduled).
    static std::int64_t thread_cpu_ns() noexcept {
        timespec ts;
#if defined(CLOCK_THREAD_CPUTIME_ID)
        if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
            return std::int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
        return 0;
    }

    // Raw cycle counter where one is readable from user space. Used only by
    // microbenchmarks that need sub-clock-granularity resolution.
    static GE_ALWAYS_INLINE std::uint64_t cycles() noexcept {
#if defined(GE_ARCH_ARM64)
        std::uint64_t v;
        asm volatile("mrs %0, cntvct_el0" : "=r"(v));
        return v;
#elif defined(GE_ARCH_X86_64)
        std::uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (std::uint64_t(hi) << 32) | lo;
#else
        return std::uint64_t(now_ns());
#endif
    }

    static ClockProbe probe(int iters = 4096) noexcept;
};

// RAII span timer. Writes the elapsed nanoseconds through a caller-supplied
// pointer so it can feed a histogram without any allocation.
class ScopedNanos {
public:
    explicit ScopedNanos(std::int64_t* out) noexcept : out_(out), t0_(Clock::now_ns()) {}
    ~ScopedNanos() noexcept { *out_ = Clock::now_ns() - t0_; }
    ScopedNanos(const ScopedNanos&) = delete;
    ScopedNanos& operator=(const ScopedNanos&) = delete;
private:
    std::int64_t* out_;
    std::int64_t  t0_;
};

// Drift-free fixed-rate scheduler: deadlines are computed from an absolute
// origin, so a slow tick is absorbed rather than shifting every later tick.
class TickScheduler {
public:
    // hz must be positive. Integer division by zero is undefined behaviour and
    // the two targets disagree about it in the worst possible way: arm64 `sdiv`
    // returns 0 without trapping, so the loop free-runs at maximum rate (18.6M
    // ticks in 3s, observed) and silently sheds everything, while x86-64 `idiv`
    // raises #DE and the process dies with SIGFPE. Neither is a diagnosis you
    // want to make from a bug report, so it is checked here rather than left to
    // each caller.
    TickScheduler(int hz, std::int64_t origin_ns) noexcept
        : period_ns_(0), origin_ns_(origin_ns) {
        GE_CHECK(hz > 0);
        period_ns_ = 1'000'000'000 / hz;
    }

    std::int64_t period_ns() const noexcept { return period_ns_; }
    std::int64_t deadline_of(std::uint64_t tick) const noexcept {
        return origin_ns_ + std::int64_t(tick) * period_ns_;
    }
    // Ticks skipped because the previous tick overran; reported, never hidden.
    std::uint64_t catch_up(std::uint64_t next_tick, std::int64_t now_ns) const noexcept {
        const std::int64_t behind = now_ns - deadline_of(next_tick);
        return behind <= 0 ? 0 : std::uint64_t(behind / period_ns_);
    }
private:
    std::int64_t period_ns_;
    std::int64_t origin_ns_;
};

} // namespace ge::core
