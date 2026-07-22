#include "ge/metrics/perf_counters.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(GE_OS_LINUX)
  #include <asm/unistd.h>
  #include <linux/perf_event.h>
  #include <sys/ioctl.h>
  #include <unistd.h>
#endif

namespace ge::metrics {

#if defined(GE_OS_LINUX)
namespace {

// perf_event_open has no glibc wrapper; it is invoked through syscall(2).
long perf_event_open(perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return ::syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

struct CounterSpec { std::uint32_t type; std::uint64_t config; const char* name; };

constexpr CounterSpec kSpecs[] = {
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,        "cycles"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,      "instructions"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES,  "cache-references"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES,      "cache-misses"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, "branches"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES,     "branch-misses"},
};

int read_paranoid() {
    std::FILE* f = std::fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (!f) return -99;
    int v = -99;
    if (std::fscanf(f, "%d", &v) != 1) v = -99;
    std::fclose(f);
    return v;
}

} // namespace

PerfCounters::PerfCounters() {
    for (int i = 0; i < kCount; ++i) fds_[i] = -1;

    // The counters are opened as a *group* led by cycles, so the kernel
    // schedules them together. Without that, if the PMU has fewer slots than
    // counters requested, the kernel multiplexes them and the ratios below are
    // computed from numbers taken over different intervals.
    for (int i = 0; i < kCount; ++i) {
        perf_event_attr attr{};
        attr.size           = sizeof(attr);
        attr.type           = kSpecs[i].type;
        attr.config         = kSpecs[i].config;
        attr.disabled       = (i == 0) ? 1 : 0;      // leader starts disabled
        attr.exclude_kernel = 1;                     // user-space only
        attr.exclude_hv     = 1;
        attr.inherit        = 1;                     // count child threads too
        attr.read_format    = 0;

        const int group = (i == 0) ? -1 : fds_[0];
        fds_[i] = int(perf_event_open(&attr, 0 /*this process*/, -1 /*any cpu*/, group, 0));
        if (fds_[i] < 0) {
            const int e = errno;
            char buf[256];
            const int paranoid = read_paranoid();
            if (e == EACCES || e == EPERM) {
                std::snprintf(buf, sizeof buf,
                    "perf_event_open denied (%s); kernel.perf_event_paranoid is %d, "
                    "needs <= 2. Try: sudo sysctl -w kernel.perf_event_paranoid=1",
                    std::strerror(e), paranoid);
            } else if (e == ENOENT || e == ENODEV || e == EOPNOTSUPP) {
                std::snprintf(buf, sizeof buf,
                    "counter '%s' not available (%s) -- common on virtualised CPUs "
                    "without a passthrough PMU", kSpecs[i].name, std::strerror(e));
            } else {
                std::snprintf(buf, sizeof buf, "perf_event_open('%s'): %s",
                              kSpecs[i].name, std::strerror(e));
            }
            reason_ = buf;
            for (int j = 0; j <= i; ++j) if (fds_[j] >= 0) { ::close(fds_[j]); fds_[j] = -1; }
            return;
        }
    }
    available_ = true;
}

PerfCounters::~PerfCounters() {
    for (int i = 0; i < kCount; ++i) if (fds_[i] >= 0) ::close(fds_[i]);
}

void PerfCounters::start() noexcept {
    if (!available_) return;
    ::ioctl(fds_[0], PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
    ::ioctl(fds_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

void PerfCounters::stop() noexcept {
    if (!available_) return;
    ::ioctl(fds_[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

PerfSample PerfCounters::read() const noexcept {
    PerfSample s;
    if (!available_) return s;
    std::uint64_t v[kCount] = {};
    for (int i = 0; i < kCount; ++i)
        if (::read(fds_[i], &v[i], sizeof v[i]) != sizeof v[i]) return s;
    s.cycles = v[0]; s.instructions = v[1];
    s.cache_references = v[2]; s.cache_misses = v[3];
    s.branches = v[4]; s.branch_misses = v[5];
    s.valid = true;
    return s;
}

std::string describe_perf_availability() {
    PerfCounters c;
    if (c.available()) return "available (perf_event_open, user-space counts)";
    return "unavailable -- " + c.unavailable_reason();
}

#else   // not Linux

PerfCounters::PerfCounters() {
    for (int i = 0; i < kCount; ++i) fds_[i] = -1;
    reason_ = "hardware counters need Linux perf_event_open; macOS exposes no "
              "unprivileged equivalent (kperf is private API and entitlement-gated)";
}
PerfCounters::~PerfCounters() = default;
void PerfCounters::start() noexcept {}
void PerfCounters::stop()  noexcept {}
PerfSample PerfCounters::read() const noexcept { return {}; }

std::string describe_perf_availability() {
    return "unavailable on this platform (Linux perf_event_open only)";
}

#endif

} // namespace ge::metrics
