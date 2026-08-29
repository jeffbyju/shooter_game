#pragma once
// CPU topology: what cores exist, how they are grouped, and how to stay on one.
//
// Why this matters more than it looks. A thread that migrates between cores
// loses its L1 and L2 working set and has to fault it back in from L3 or
// memory -- roughly 100ns per line. For a
// thread whose hot data is a ring buffer and a few connection objects, a single
// migration can cost more than the thousand messages it was about to handle.
//
// On a multi-socket machine it is worse. Memory is attached to a socket, and
// reaching memory attached to *another* socket costs roughly twice as much.
// A thread pinned to node 1 whose buffers were allocated on node 0 pays that
// penalty on every access, forever, invisibly.
//
// So the goal is: put each thread on a known core, and put the memory it uses
// on the same NUMA node. Linux exposes both; macOS exposes neither, and this
// header says so rather than pretending.
//
// Background, if NUMA is unfamiliar: The
// one rule worth knowing before reading this file is first-touch -- Linux
// allocates a physical page on the node of the thread that first *writes* it,
// not the one that allocated it -- which is why pinning must happen before
// prefaulting, and why bind_memory_to_node() is documented as "call before
// touching the pages".
#include <cstdint>
#include <string>
#include <vector>

#include "ge/core/platform.hpp"

namespace ge::core {

struct CpuInfo {
    int cpu          = -1;   // logical CPU id as the OS numbers it
    int core         = -1;   // physical core (two hyperthreads share one)
    int socket       = -1;   // package
    int numa_node    = -1;
    bool is_hyperthread_sibling = false;   // second thread on an already-seen core
};

struct Topology {
    std::vector<CpuInfo> cpus;
    int  numa_nodes      = 1;
    int  physical_cores  = 0;
    int  logical_cpus    = 0;
    bool has_smt         = false;
    bool detected        = false;   // false => everything below is a guess

    // Logical CPUs on one NUMA node, excluding hyperthread siblings.
    //
    // Siblings are excluded on purpose. Two threads on one physical core share
    // its execution units, L1 and L2; for latency-sensitive work that is a way
    // to halve your throughput while doubling your thread count. Filling
    // physical cores first, and only using siblings if you run out, is almost
    // always what you want.
    std::vector<int> primary_cpus_on_node(int node) const;

    // A placement plan: one CPU per thread, filling physical cores on the
    // first NUMA node before spilling to the next.
    std::vector<int> plan(int thread_count) const;

    std::string describe() const;
};

// Reads the topology. On Linux this parses /sys/devices/system/cpu; on macOS it
// fills in what sysctl offers and sets detected = false.
const Topology& topology();

// Pins the calling thread to one logical CPU. Returns false when the platform
// cannot do it -- notably Apple Silicon, where THREAD_AFFINITY_POLICY is
// ignored. A false return is reported as "not pinned" rather than swallowed,
// because a benchmark that claims a control it does not have is lying.
bool pin_to_cpu(int cpu) noexcept;

// Restricts the calling thread to every CPU on one NUMA node, without choosing
// a specific core. Useful for threads that should stay local to their memory
// but do not need a fixed core.
bool pin_to_numa_node(int node) noexcept;

// Which NUMA node the calling thread is running on right now, or -1.
int current_numa_node() noexcept;

// Binds an already-mapped range to a NUMA node, so its pages are faulted in
// from that node's memory. Call before touching the pages: Linux allocates a
// physical page on first touch, and moving one afterwards means copying it.
bool bind_memory_to_node(void* addr, std::size_t bytes, int node) noexcept;

} // namespace ge::core
