#pragma once
// io_uring readiness backend (Linux).
//
// What io_uring changes
// ---------------------
// epoll costs one syscall per wait, plus one epoll_ctl per interest change. A
// syscall is ~1-12us here, and an event
// loop that wakes 100k times a second is spending real time in the kernel
// boundary rather than on work.
//
// io_uring replaces that boundary with two shared ring buffers mapped into both
// the kernel's and the process's address space: a submission queue the process
// writes and a completion queue it reads. Requests are placed in memory, not
// passed as syscall arguments, so many can be handed over in one io_uring_enter
// -- or, with SQPOLL, in none at all, because a kernel thread polls the ring.
//
// Scope of this implementation
// ----------------------------
// This is deliberately a *readiness* backend: it submits IORING_OP_POLL_ADD and
// reports "this fd is readable/writable", exactly like epoll, so it drops into
// the existing Reactor interface and the whole I/O path is unchanged.
//
// That is the smaller half of what io_uring offers. The larger half is
// submitting the reads and writes themselves (IORING_OP_RECV/SEND) with
// registered buffers, so data is transferred without any syscall on the data
// path. That is a rewrite of the I/O path rather than a backend swap, and it is
// not done here. Stating the ceiling is more useful than implying there is none:
// expect this to save the interest-change syscalls and batch the waits, not to
// eliminate the per-message ones.
//
// What this is NOT
// ----------------
// io_uring is not kernel bypass, and the two are routinely conflated. A packet
// handled here still traverses the entire kernel path: NIC, driver, softirq,
// the TCP/IP stack, the socket buffer. io_uring changes only how the process
// *asks* the kernel to do that work -- shared rings instead of a syscall per
// operation.
//
// Kernel bypass (AF_XDP, DPDK, Solarflare Onload) removes the kernel from the
// data path: the NIC DMAs frames straight into user memory and you supply the
// TCP stack yourself. That is a rewrite of this layer, not a backend swap, and
// it is not implemented here. 
//
// No liburing dependency -- the syscalls are made directly, consistent with the
// rest of the project having no third-party code.
#include <cstdint>

#include "ge/core/error.hpp"
#include "ge/core/platform.hpp"

namespace ge::net {

// True when the running kernel supports io_uring at all. Checked at runtime,
// not compile time: a binary built on a modern kernel routinely runs on an
// older one, and cloud images vary more than anything else in this project.
bool uring_available() noexcept;

// A short description of what was detected, for the run report.
const char* uring_status() noexcept;

} // namespace ge::net
