#pragma once
// Thin RAII wrapper over a socket descriptor plus the option set that actually
// matters for latency. Every setsockopt here has a reason recorded next to it;
// none of them is cargo cult.
#include <cstdint>
#include <string>
#include <utility>

#include <sys/types.h>
#include <sys/uio.h>

#include "ge/core/error.hpp"
#include "ge/core/platform.hpp"

namespace ge::net {

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { close(); }

    Fd(const Fd&)            = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) { close(); fd_ = std::exchange(o.fd_, -1); }
        return *this;
    }

    int  get()   const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int  release() noexcept     { return std::exchange(fd_, -1); }
    void close() noexcept;

private:
    int fd_ = -1;
};

// How a read/write ended. Distinguishing "would block" from "peer gone" from
// "real error" at this layer keeps the event loop free of errno handling.
enum class IoResult : std::uint8_t { Ok, WouldBlock, Closed, Error };

struct IoOutcome {
    IoResult result = IoResult::Ok;
    ssize_t  bytes  = 0;
    int      err    = 0;
};

bool set_nonblocking(int fd) noexcept;

// Nagle batches small writes for up to 40ms waiting for an ACK. For a protocol
// whose whole purpose is to emit one small frame per tick, that is a latency
// bug, not an optimisation.
bool set_nodelay(int fd) noexcept;

bool set_reuseaddr(int fd) noexcept;
bool set_reuseport(int fd) noexcept;

// Sizing SO_SNDBUF/SO_RCVBUF explicitly makes the backpressure point a property
// of the configuration instead of a property of whichever kernel we landed on.
bool set_buffer_sizes(int fd, int send_bytes, int recv_bytes) noexcept;

// Suppress SIGPIPE per-socket where the platform allows it, so a peer vanishing
// mid-write returns EPIPE to the caller instead of killing the process.
bool set_nosigpipe(int fd) noexcept;

// Both return the descriptor or the reason they could not get one. The caller
// cannot use the Fd without first dealing with the failure case.
core::Result<Fd> listen_tcp(const char* host, std::uint16_t port, int backlog);

// A listener that shares a port with other listeners via SO_REUSEPORT.
//
// The kernel then hashes each incoming connection's 4-tuple to one of the
// listeners and queues it there. That replaces the whole acceptor-thread design:
// no single thread accepting, no queue handing descriptors across a thread
// boundary, no wakeup to tell the I/O thread something arrived. Each I/O thread
// simply accepts its own share.
//
// It is Linux-specific in the way that matters. macOS and the BSDs implement
// SO_REUSEPORT as "last binder wins" for TCP rather than load balancing, so
// enabling it there would send every connection to one thread. Callers must
// check reuseport_balances_connections() rather than assuming.
core::Result<Fd> listen_tcp_shared(const char* host, std::uint16_t port, int backlog);

// True only where the kernel actually distributes connections across listeners
// sharing a port. Linux >= 3.9 does; Darwin does not.
bool reuseport_balances_connections() noexcept;
core::Result<Fd> connect_tcp(const char* host, std::uint16_t port, bool nonblocking);

IoOutcome read_some(int fd, void* buf, std::size_t n) noexcept;
IoOutcome write_some(int fd, const void* buf, std::size_t n) noexcept;
IoOutcome writev_some(int fd, const iovec* iov, int iovcnt) noexcept;

std::uint16_t local_port(int fd) noexcept;

} // namespace ge::net
