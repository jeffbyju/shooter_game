#pragma once
// Readiness notification over kqueue (Darwin/BSD) and epoll (Linux).
//
// Two deliberate choices:
//
// * Level-triggered. Edge-triggered saves a syscall per socket but obliges
//   every handler to drain to EAGAIN, and one handler that forgets stalls a
//   connection forever with no visible error. Level-triggered lets the loop cap
//   per-connection work and move on, which bounds head-of-line blocking between
//   connections on the same thread -- that fairness matters more here than one
//   syscall.
//
// * Write interest is registered only while output is pending. A socket with an
//   empty send queue is writable essentially always, so leaving write interest
//   armed turns the event loop into a spin loop that burns a core doing nothing.
#include <cstdint>
#include <string>
#include <vector>
#include <cstddef>

#include "ge/core/platform.hpp"
#include "ge/net/socket.hpp"

namespace ge::net {

enum Interest : std::uint8_t {
    kNone  = 0,
    kRead  = 1u << 0,
    kWrite = 1u << 1,
};

struct Event {
    std::uint64_t token    = 0;   // opaque caller key, usually a connection handle
    bool          readable = false;
    bool          writable = false;
    bool          hangup   = false;
    bool          error    = false;
};

// Which kernel interface the loop waits on.
enum class Backend : std::uint8_t {
    Auto,     // io_uring if the kernel offers it, else epoll/kqueue
    Poll,     // force epoll (Linux) / kqueue (Darwin)
    Uring,    // force io_uring; open() fails if unavailable
};

class Reactor {
public:
    Reactor() = default;
    ~Reactor();
    Reactor(const Reactor&)            = delete;
    Reactor& operator=(const Reactor&) = delete;

    // `backend` defaults to Poll rather than Auto: the io_uring path has not yet
    // been validated on real hardware, and defaulting to an unvalidated code
    // path would be choosing novelty over correctness. See docs/tutorial/14.
    core::Status open(Backend backend = Backend::Poll);

    // What this reactor actually ended up using, for the run report.
    const char* backend_name() const noexcept;

    bool add(int fd, std::uint64_t token, std::uint8_t interest) noexcept;
    bool modify(int fd, std::uint64_t token, std::uint8_t interest) noexcept;
    bool remove(int fd) noexcept;

    // timeout_ms < 0 blocks indefinitely; 0 polls. Returns the event count, or
    // -1 on a real error (EINTR is retried internally).
    int poll(Event* out, int max_events, int timeout_ms) noexcept;

    // Cross-thread wakeup: the acceptor uses it to tell an I/O thread that a new
    // connection is waiting in its handoff queue, and the shutdown path uses it
    // to break a blocking poll.
    bool wake() noexcept;

private:
    core::Status open_poll();
#if defined(GE_OS_LINUX)
    core::Status open_uring();
    int  poll_uring(Event* out, int max_events, int timeout_ms) noexcept;
    bool submit_poll(int fd, std::uint64_t token, std::uint8_t interest) noexcept;
    void rearm(int fd) noexcept;

    // Shared submission/completion rings, mapped into both address spaces.
    struct Ring {
        void*          sq_mmap    = nullptr;   size_t sq_bytes = 0;
        void*          cq_mmap    = nullptr;   size_t cq_bytes = 0;
        void*          sqe_mmap   = nullptr;   size_t sqe_bytes = 0;
        unsigned*      sq_head    = nullptr;
        unsigned*      sq_tail    = nullptr;
        unsigned*      sq_mask    = nullptr;
        unsigned*      sq_array   = nullptr;
        unsigned*      cq_head    = nullptr;
        unsigned*      cq_tail    = nullptr;
        unsigned*      cq_mask    = nullptr;
        void*          sqes       = nullptr;
        void*          cqes       = nullptr;
        unsigned       sq_entries = 0;
    };
    Ring     ring_{};
    Fd       ring_fd_;
    unsigned pending_submits_ = 0;
    // Interest per fd, so a completion can re-arm the same registration.
    // Single-shot poll fires once and is gone; this is what puts it back.
    std::vector<std::uint8_t>  interest_;
    std::vector<std::uint64_t> tokens_;
    bool use_uring_ = false;
#endif
    Fd   mux_;
    Fd   wake_read_;
    Fd   wake_write_;
};

inline constexpr std::uint64_t kWakeToken = ~std::uint64_t{0};

} // namespace ge::net
