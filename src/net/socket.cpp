#include "ge/net/socket.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ge::net {
namespace {

bool set_flag(int fd, int level, int opt, int value) noexcept {
    return ::setsockopt(fd, level, opt, &value, sizeof value) == 0;
}

IoResult classify(ssize_t n, int e) noexcept {
    if (n > 0) return IoResult::Ok;
    if (n == 0) return IoResult::Closed;                     // orderly shutdown
    if (e == EAGAIN || e == EWOULDBLOCK) return IoResult::WouldBlock;
    if (e == ECONNRESET || e == EPIPE)   return IoResult::Closed;
    return IoResult::Error;
}

} // namespace

void Fd::close() noexcept {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool set_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool set_nodelay(int fd)   noexcept { return set_flag(fd, IPPROTO_TCP, TCP_NODELAY, 1); }
bool set_reuseaddr(int fd) noexcept { return set_flag(fd, SOL_SOCKET, SO_REUSEADDR, 1); }

bool set_reuseport(int fd) noexcept {
#ifdef SO_REUSEPORT
    return set_flag(fd, SOL_SOCKET, SO_REUSEPORT, 1);
#else
    (void)fd; return false;
#endif
}

bool set_buffer_sizes(int fd, int send_bytes, int recv_bytes) noexcept {
    bool ok = true;
    if (send_bytes > 0) ok &= set_flag(fd, SOL_SOCKET, SO_SNDBUF, send_bytes);
    if (recv_bytes > 0) ok &= set_flag(fd, SOL_SOCKET, SO_RCVBUF, recv_bytes);
    return ok;
}

bool set_nosigpipe(int fd) noexcept {
#ifdef SO_NOSIGPIPE
    return set_flag(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);   // Darwin
#else
    (void)fd; return true;                              // Linux uses MSG_NOSIGNAL per call
#endif
}

namespace {

core::Result<Fd> resolve_and(const char* host, std::uint16_t port, bool server, bool nonblocking) {
    char portstr[16];
    std::snprintf(portstr, sizeof portstr, "%u", unsigned(port));

    addrinfo hints{};
    hints.ai_family   = AF_INET;          // v4 only: keeps the address path branch-free
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = server ? AI_PASSIVE : 0;

    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) return core::fail("getaddrinfo", ::gai_strerror(rc));

    Fd fd(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (!fd.valid()) {
        const auto e = core::fail("socket", std::strerror(errno));
        ::freeaddrinfo(res);
        return e;
    }
    set_nosigpipe(fd.get());
    set_nodelay(fd.get());

    if (server) {
        set_reuseaddr(fd.get());
        if (::bind(fd.get(), res->ai_addr, res->ai_addrlen) != 0) {
            const auto e = core::fail("bind", std::strerror(errno));
            ::freeaddrinfo(res);
            return e;
        }
    } else {
        if (nonblocking) set_nonblocking(fd.get());
        if (::connect(fd.get(), res->ai_addr, res->ai_addrlen) != 0) {
            const int e = errno;
            if (!(nonblocking && (e == EINPROGRESS || e == EALREADY))) {
                const auto failed = core::fail("connect", std::strerror(e));
                ::freeaddrinfo(res);
                return failed;
            }
        }
    }
    ::freeaddrinfo(res);
    return fd;
}

} // namespace

namespace {
// Same as resolve_and(server=true) but sets SO_REUSEPORT *before* bind, which
// is the only point at which the kernel will accept it.
core::Result<Fd> resolve_shared(const char* host, std::uint16_t port) {
    char portstr[16];
    std::snprintf(portstr, sizeof portstr, "%u", unsigned(port));
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) return core::fail("getaddrinfo", ::gai_strerror(rc));

    Fd fd(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (!fd.valid()) {
        const auto e = core::fail("socket", std::strerror(errno));
        ::freeaddrinfo(res);
        return e;
    }
    set_nosigpipe(fd.get());
    set_nodelay(fd.get());
    set_reuseaddr(fd.get());
    if (!set_reuseport(fd.get())) {
        const auto e = core::fail("setsockopt(SO_REUSEPORT)", std::strerror(errno));
        ::freeaddrinfo(res);
        return e;
    }
    if (::bind(fd.get(), res->ai_addr, res->ai_addrlen) != 0) {
        const auto e = core::fail("bind", std::strerror(errno));
        ::freeaddrinfo(res);
        return e;
    }
    ::freeaddrinfo(res);
    return fd;
}
} // namespace

core::Result<Fd> listen_tcp(const char* host, std::uint16_t port, int backlog) {
    auto fd = resolve_and(host, port, true, false);
    if (!fd) return fd;
    if (::listen(fd->get(), backlog) != 0) return core::fail("listen", std::strerror(errno));
    set_nonblocking(fd->get());
    return fd;
}

core::Result<Fd> connect_tcp(const char* host, std::uint16_t port, bool nonblocking) {
    return resolve_and(host, port, false, nonblocking);
}

bool reuseport_balances_connections() noexcept {
#if defined(GE_OS_LINUX) && defined(SO_REUSEPORT)
    return true;
#else
    return false;
#endif
}

core::Result<Fd> listen_tcp_shared(const char* host, std::uint16_t port, int backlog) {
#if !defined(SO_REUSEPORT)
    (void)host; (void)port; (void)backlog;
    return core::fail("SO_REUSEPORT not available on this platform");
#else
    auto fd = resolve_shared(host, port);
    if (!fd) return fd;
    if (::listen(fd->get(), backlog) != 0) return core::fail("listen", std::strerror(errno));
    set_nonblocking(fd->get());
    return fd;
#endif
}

IoOutcome read_some(int fd, void* buf, std::size_t n) noexcept {
    for (;;) {
        const ssize_t r = ::read(fd, buf, n);
        if (r < 0 && errno == EINTR) continue;       // retry, never surface EINTR upward
        return {classify(r, errno), r, r < 0 ? errno : 0};
    }
}

IoOutcome write_some(int fd, const void* buf, std::size_t n) noexcept {
    for (;;) {
#if defined(MSG_NOSIGNAL)
        const ssize_t r = ::send(fd, buf, n, MSG_NOSIGNAL);
#else
        const ssize_t r = ::write(fd, buf, n);
#endif
        if (r < 0 && errno == EINTR) continue;
        // A 0-byte write is not a close; only a 0-byte *read* is.
        if (r == 0) return {IoResult::WouldBlock, 0, 0};
        return {classify(r, errno), r, r < 0 ? errno : 0};
    }
}

IoOutcome writev_some(int fd, const iovec* iov, int iovcnt) noexcept {
    for (;;) {
        const ssize_t r = ::writev(fd, iov, iovcnt);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) return {IoResult::WouldBlock, 0, 0};
        return {classify(r, errno), r, r < 0 ? errno : 0};
    }
}

std::uint16_t local_port(int fd) noexcept {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

} // namespace ge::net
