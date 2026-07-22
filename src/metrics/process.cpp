#include "ge/metrics/process.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <sys/time.h>

#include "ge/core/clock.hpp"

#if defined(GE_OS_MACOS)
  #include <mach/mach.h>
  #include <mach/mach_init.h>
  #include <mach/task.h>
#elif defined(GE_OS_LINUX)
  #include <unistd.h>
#endif

namespace ge::metrics {
namespace {

std::int64_t tv_ns(const timeval& tv) noexcept {
    return std::int64_t(tv.tv_sec) * 1'000'000'000 + std::int64_t(tv.tv_usec) * 1000;
}

} // namespace

ProcessSample sample_process() noexcept {
    ProcessSample s;
    s.wall_ns = core::Clock::now_ns();

    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) == 0) {
        s.cpu_user_ns        = tv_ns(ru.ru_utime);
        s.cpu_system_ns      = tv_ns(ru.ru_stime);
        s.minor_faults       = std::uint64_t(ru.ru_minflt);
        s.major_faults       = std::uint64_t(ru.ru_majflt);
        s.vol_ctx_switches   = std::uint64_t(ru.ru_nvcsw);
        s.invol_ctx_switches = std::uint64_t(ru.ru_nivcsw);
#if defined(GE_OS_MACOS)
        s.peak_rss_bytes = std::uint64_t(ru.ru_maxrss);          // Darwin reports bytes
#else
        s.peak_rss_bytes = std::uint64_t(ru.ru_maxrss) * 1024;   // Linux reports KiB
#endif
    }

#if defined(GE_OS_MACOS)
    // mach_task_basic_info is the authoritative current RSS on Darwin; there is
    // no /proc to read and ru_maxrss only gives the high-water mark.
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        s.rss_bytes     = info.resident_size;
        s.virtual_bytes = info.virtual_size;
        if (info.resident_size_max > s.peak_rss_bytes) s.peak_rss_bytes = info.resident_size_max;
    }
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t tcount = 0;
    if (::task_threads(::mach_task_self(), &threads, &tcount) == KERN_SUCCESS) {
        s.thread_count = tcount;
        for (mach_msg_type_number_t i = 0; i < tcount; ++i)
            ::mach_port_deallocate(::mach_task_self(), threads[i]);
        ::vm_deallocate(::mach_task_self(), vm_address_t(threads), tcount * sizeof(thread_act_t));
    }
#elif defined(GE_OS_LINUX)
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long vsz = 0, rss = 0;
        if (std::fscanf(f, "%lu %lu", &vsz, &rss) == 2) {
            const auto page = std::uint64_t(::sysconf(_SC_PAGESIZE));
            s.virtual_bytes = std::uint64_t(vsz) * page;
            s.rss_bytes     = std::uint64_t(rss) * page;
        }
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            unsigned t = 0;
            if (std::sscanf(line, "Threads: %u", &t) == 1) { s.thread_count = t; break; }
        }
        std::fclose(f);
    }
#endif
    return s;
}

ResourceDelta diff(const ProcessSample& a, const ProcessSample& b) noexcept {
    ResourceDelta d;
    d.wall_ns            = b.wall_ns - a.wall_ns;
    d.cpu_ns             = b.cpu_ns() - a.cpu_ns();
    d.minor_faults       = b.minor_faults - a.minor_faults;
    d.major_faults       = b.major_faults - a.major_faults;
    d.vol_ctx_switches   = b.vol_ctx_switches - a.vol_ctx_switches;
    d.invol_ctx_switches = b.invol_ctx_switches - a.invol_ctx_switches;
    d.rss_bytes          = b.rss_bytes;
    d.peak_rss_bytes     = b.peak_rss_bytes;
    return d;
}

std::string format_bytes(std::uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    char buf[48];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.2f %s", v, kUnits[u]);
    return buf;
}

} // namespace ge::metrics
