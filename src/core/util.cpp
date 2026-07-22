#include "ge/core/clock.hpp"
#include "ge/core/thread.hpp"

#include <algorithm>
#include <array>
#include <pthread.h>
#include <unistd.h>

#if defined(GE_OS_MACOS)
  #include <mach/mach.h>
  #include <mach/thread_policy.h>
  #include <pthread/qos.h>
  #include <sys/sysctl.h>
#elif defined(GE_OS_LINUX)
  #include <sched.h>
#endif

namespace ge::core {

ClockProbe Clock::probe(int iters) noexcept {
    // Call overhead has to be measured in batches, not as the delta between two
    // consecutive reads. The clock's own granularity here (~41ns) exceeds the
    // call cost, so consecutive reads usually return the *same* value and a
    // per-call delta reports a nonsensical 0ns. Timing a batch of calls and
    // dividing puts the answer well above the granularity floor; the best of
    // several batches is taken because noise is additive.
    if (iters < 64) iters = 64;
    std::array<std::int64_t, 16> batches{};
    for (std::size_t r = 0; r < batches.size(); ++r) {
        const std::int64_t t0 = now_ns();
        std::int64_t acc = 0;
        for (int i = 0; i < iters; ++i) acc += now_ns();
        const std::int64_t t1 = now_ns();
        asm volatile("" : : "r"(acc) : "memory");     // keep the loop alive
        batches[r] = (t1 - t0) / iters;
    }
    std::sort(batches.begin(), batches.end());
    const std::int64_t overhead = batches.front();

    // Granularity: the smallest non-zero delta this clock will ever report. Any
    // latency figure near it is an artefact of the instrument, not a result.
    std::int64_t gran = INT64_MAX;
    for (int i = 0; i < 64; ++i) {
        const std::int64_t a = now_ns();
        std::int64_t b = a;
        while (b == a) b = now_ns();
        gran = std::min(gran, b - a);
    }
    return {overhead, gran == INT64_MAX ? 0 : gran};
}

void set_thread_name(const char* name) noexcept {
#if defined(GE_OS_MACOS)
    ::pthread_setname_np(name);          // Darwin names the calling thread only
#elif defined(GE_OS_LINUX)
    ::pthread_setname_np(::pthread_self(), name);
#endif
}

bool pin_to_core(unsigned core) noexcept {
#if defined(GE_OS_LINUX)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#elif defined(GE_OS_MACOS)
    // THREAD_AFFINITY_POLICY is advisory on Intel Macs and ignored outright on
    // Apple Silicon. Setting the hint and then reporting "pinned" would be a
    // lie, so we set it and return the truth: not pinned.
    thread_affinity_policy_data_t policy{int(core) + 1};
    (void)::thread_policy_set(::pthread_mach_thread_np(::pthread_self()),
                              THREAD_AFFINITY_POLICY,
                              reinterpret_cast<thread_policy_t>(&policy),
                              THREAD_AFFINITY_POLICY_COUNT);
    return false;
#else
    (void)core;
    return false;
#endif
}

void request_perf_core(bool high) noexcept {
#if defined(GE_OS_MACOS)
    // On an asymmetric-core machine this is the only lever that works:
    // USER_INTERACTIVE keeps the thread on a P-core, UTILITY lets it drift to
    // an E-core. Without it the scheduler may migrate a hot I/O thread onto an
    // efficiency core mid-run, which shows up as a bimodal latency tail.
    ::pthread_set_qos_class_self_np(high ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_UTILITY, 0);
#else
    (void)high;
#endif
}

unsigned hardware_cores() noexcept {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

unsigned performance_cores() noexcept {
#if defined(GE_OS_MACOS)
    // Apple Silicon: perflevel0 is the P-cluster. Benchmarks are sized to the
    // P-core count, because spilling threads onto E-cores halves per-thread
    // throughput and makes a scaling curve look like a contention wall when it
    // is really a core-type change.
    int v = 0;
    std::size_t sz = sizeof v;
    if (::sysctlbyname("hw.perflevel0.logicalcpu", &v, &sz, nullptr, 0) == 0 && v > 0)
        return unsigned(v);
#endif
    return hardware_cores();
}

} // namespace ge::core
