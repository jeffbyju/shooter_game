#pragma once
#include <cstdint>
#include <string>
#include <thread>
#include "ge/core/platform.hpp"

namespace ge::core {

// Naming threads makes Instruments / perf / `sample` output readable, which is
// the difference between a flame graph you can act on and one you can't.
void set_thread_name(const char* name) noexcept;

// Pin the calling thread to a logical core.
//
// Linux: real sched_setaffinity, returns true on success.
// Darwin: THREAD_AFFINITY_POLICY is only an advisory hint and is a no-op on
// Apple Silicon entirely, so this returns false and the caller reports that
// results were gathered unpinned. We do not pretend to a control we lack.
bool pin_to_core(unsigned core) noexcept;

// Ask the scheduler to keep this thread on a performance core (Darwin QoS).
void request_perf_core(bool high) noexcept;

unsigned hardware_cores() noexcept;
unsigned performance_cores() noexcept;   // == hardware_cores() where not applicable

} // namespace ge::core
