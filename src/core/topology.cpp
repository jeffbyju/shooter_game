#include "ge/core/topology.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>

#include <pthread.h>
#include <unistd.h>

#if defined(GE_OS_LINUX)
  #include <sched.h>
  #include <sys/syscall.h>
#elif defined(GE_OS_MACOS)
  #include <mach/mach.h>
  #include <mach/thread_policy.h>
  #include <sys/sysctl.h>
#endif

namespace ge::core {
namespace {

#if defined(GE_OS_LINUX)
// Reads a single integer out of a sysfs file. Returns fallback if absent.
int read_int(const char* fmt, int a, int fallback) {
    char path[256];
    std::snprintf(path, sizeof path, fmt, a);
    std::FILE* f = std::fopen(path, "r");
    if (!f) return fallback;
    int v = fallback;
    if (std::fscanf(f, "%d", &v) != 1) v = fallback;
    std::fclose(f);
    return v;
}

// NUMA node of a CPU: sysfs exposes this as a symlink node<N> inside the CPU's
// directory, so it is found by probing rather than reading a value.
int numa_node_of(int cpu) {
    for (int node = 0; node < 64; ++node) {
        char path[256];
        std::snprintf(path, sizeof path,
                      "/sys/devices/system/node/node%d/cpu%d", node, cpu);
        if (::access(path, F_OK) == 0) return node;
    }
    return 0;
}

Topology detect_linux() {
    Topology t;
    const int n = int(::sysconf(_SC_NPROCESSORS_ONLN));
    if (n <= 0) return t;

    std::set<std::pair<int, int>> seen_cores;   // (socket, core)
    std::set<int> nodes;
    for (int cpu = 0; cpu < n; ++cpu) {
        CpuInfo c;
        c.cpu       = cpu;
        c.core      = read_int("/sys/devices/system/cpu/cpu%d/topology/core_id", cpu, cpu);
        c.socket    = read_int("/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu, 0);
        c.numa_node = numa_node_of(cpu);
        c.is_hyperthread_sibling = !seen_cores.insert({c.socket, c.core}).second;
        nodes.insert(c.numa_node);
        t.cpus.push_back(c);
    }
    t.logical_cpus   = n;
    t.physical_cores = int(seen_cores.size());
    t.numa_nodes     = int(nodes.size());
    t.has_smt        = t.logical_cpus > t.physical_cores;
    t.detected       = true;
    return t;
}
#endif

#if defined(GE_OS_MACOS)
int sysctl_int(const char* name, int fallback) {
    int v = 0;
    std::size_t sz = sizeof v;
    if (::sysctlbyname(name, &v, &sz, nullptr, 0) == 0 && v > 0) return v;
    return fallback;
}

Topology detect_macos() {
    Topology t;
    t.logical_cpus   = sysctl_int("hw.logicalcpu", int(std::thread::hardware_concurrency()));
    t.physical_cores = sysctl_int("hw.physicalcpu", t.logical_cpus);
    t.numa_nodes     = 1;                      // no NUMA on any shipping Mac
    t.has_smt        = t.logical_cpus > t.physical_cores;
    for (int cpu = 0; cpu < t.logical_cpus; ++cpu) {
        CpuInfo c;
        c.cpu = cpu; c.core = cpu; c.socket = 0; c.numa_node = 0;
        // Darwin does not expose a sibling map; with no way to tell, claiming
        // to know would be worse than admitting we do not.
        c.is_hyperthread_sibling = false;
        t.cpus.push_back(c);
    }
    // detected stays false: the CPU *count* is right, the layout is a guess,
    // and pinning does not work here anyway.
    t.detected = false;
    return t;
}
#endif

Topology detect() {
#if defined(GE_OS_LINUX)
    return detect_linux();
#elif defined(GE_OS_MACOS)
    return detect_macos();
#else
    Topology t;
    t.logical_cpus = t.physical_cores = int(std::thread::hardware_concurrency());
    return t;
#endif
}

} // namespace

const Topology& topology() {
    static const Topology t = detect();
    return t;
}

std::vector<int> Topology::primary_cpus_on_node(int node) const {
    std::vector<int> out;
    for (const auto& c : cpus)
        if (c.numa_node == node && !c.is_hyperthread_sibling) out.push_back(c.cpu);
    return out;
}

std::vector<int> Topology::plan(int thread_count) const {
    std::vector<int> out;
    out.reserve(std::size_t(std::max(0, thread_count)));

    // Physical cores on node 0, then node 1, and so on; only once every
    // physical core is taken do we start handing out hyperthread siblings.
    for (int node = 0; node < numa_nodes && int(out.size()) < thread_count; ++node)
        for (int cpu : primary_cpus_on_node(node)) {
            if (int(out.size()) >= thread_count) break;
            out.push_back(cpu);
        }
    for (const auto& c : cpus) {
        if (int(out.size()) >= thread_count) break;
        if (c.is_hyperthread_sibling) out.push_back(c.cpu);
    }
    // Still short (more threads than CPUs): wrap around rather than leave
    // threads unpinned, so the placement is at least deterministic.
    for (std::size_t i = 0; int(out.size()) < thread_count && !cpus.empty(); ++i)
        out.push_back(cpus[i % cpus.size()].cpu);
    return out;
}

std::string Topology::describe() const {
    char buf[256];
    if (!detected) {
        std::snprintf(buf, sizeof buf,
                      "%d logical CPUs (layout not detectable on this platform)", logical_cpus);
        return buf;
    }
    std::snprintf(buf, sizeof buf,
                  "%d logical CPUs, %d physical cores, %d NUMA node%s%s",
                  logical_cpus, physical_cores, numa_nodes,
                  numa_nodes == 1 ? "" : "s", has_smt ? ", SMT enabled" : "");
    return buf;
}

bool pin_to_cpu(int cpu) noexcept {
#if defined(GE_OS_LINUX)
    if (cpu < 0) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(unsigned(cpu), &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#elif defined(GE_OS_MACOS)
    // THREAD_AFFINITY_POLICY is advisory on Intel Macs and ignored entirely on
    // Apple Silicon. The hint is set because it costs nothing on the machines
    // where it does something, and false is returned because on this one it
    // does not.
    thread_affinity_policy_data_t policy{cpu + 1};
    (void)::thread_policy_set(::pthread_mach_thread_np(::pthread_self()),
                              THREAD_AFFINITY_POLICY,
                              reinterpret_cast<thread_policy_t>(&policy),
                              THREAD_AFFINITY_POLICY_COUNT);
    return false;
#else
    (void)cpu;
    return false;
#endif
}

bool pin_to_numa_node(int node) noexcept {
#if defined(GE_OS_LINUX)
    cpu_set_t set;
    CPU_ZERO(&set);
    bool any = false;
    for (const auto& c : topology().cpus)
        if (c.numa_node == node && c.cpu >= 0) { CPU_SET(unsigned(c.cpu), &set); any = true; }
    if (!any) return false;
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#else
    (void)node;
    return false;
#endif
}

int current_numa_node() noexcept {
#if defined(GE_OS_LINUX)
    unsigned cpu = 0, node = 0;
    // getcpu(2); called through syscall() so no glibc version floor is needed.
    if (::syscall(SYS_getcpu, &cpu, &node, nullptr) == 0) return int(node);
    return -1;
#else
    return -1;
#endif
}

bool bind_memory_to_node(void* addr, std::size_t bytes, int node) noexcept {
#if defined(GE_OS_LINUX)
    // mbind(2) with MPOL_BIND. Used directly rather than through libnuma to
    // avoid a dependency for one call.
    constexpr int kMpolBind = 2;
    constexpr unsigned kMoveFlag = 1u << 1;      // MPOL_MF_MOVE
    unsigned long mask = 1UL << unsigned(node);
    const long rc = ::syscall(SYS_mbind, addr, bytes, kMpolBind,
                              &mask, sizeof(mask) * 8, kMoveFlag);
    return rc == 0;
#else
    (void)addr; (void)bytes; (void)node;
    return false;
#endif
}

} // namespace ge::core
