#include "ge/net/reactor.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "ge/net/uring.hpp"

#if defined(GE_OS_LINUX)
  #include <linux/io_uring.h>
  #include <poll.h>
  #include <sys/mman.h>
  #include <sys/syscall.h>
#endif

#if defined(GE_OS_MACOS)
  #include <sys/event.h>
#elif defined(GE_OS_LINUX)
  #include <sys/epoll.h>
  #include <sys/eventfd.h>
#endif

#include "ge/core/assert.hpp"

namespace ge::net {

Reactor::~Reactor() = default;

const char* Reactor::backend_name() const noexcept {
#if defined(GE_OS_LINUX)
    return use_uring_ ? "io_uring (readiness mode, UNVALIDATED)" : "epoll";
#else
    return "kqueue";
#endif
}

core::Status Reactor::open(Backend backend) {
#if defined(GE_OS_LINUX)
    if (backend == Backend::Uring || (backend == Backend::Auto && uring_available())) {
        if (auto r = open_uring(); r) { use_uring_ = true; return r; }
        else if (backend == Backend::Uring) return r;   // asked for it explicitly
        // Auto: fall through to epoll rather than failing the server.
    }
#else
    if (backend == Backend::Uring)
        return core::fail("io_uring requested but this platform is not Linux");
#endif
    return open_poll();
}

core::Status Reactor::open_poll() {
#if defined(GE_OS_MACOS)
    mux_ = Fd(::kqueue());
#else
    mux_ = Fd(::epoll_create1(EPOLL_CLOEXEC));
#endif
    if (!mux_.valid()) return core::fail("event mux", std::strerror(errno));

#if defined(GE_OS_LINUX)
    wake_read_ = Fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake_read_.valid()) return core::fail("eventfd", std::strerror(errno));
#else
    // No eventfd on Darwin; a self-pipe is the portable equivalent and the
    // wakeup is off the hot path, so the extra fd costs nothing measurable.
    int p[2];
    if (::pipe(p) != 0) return core::fail("pipe", std::strerror(errno));
    wake_read_  = Fd(p[0]);
    wake_write_ = Fd(p[1]);
    set_nonblocking(wake_read_.get());
    set_nonblocking(wake_write_.get());
#endif
    if (!add(wake_read_.get(), kWakeToken, kRead))
        return core::fail("register wakeup", std::strerror(errno));
    return core::ok();
}

#if defined(GE_OS_MACOS)

bool Reactor::add(int fd, std::uint64_t token, std::uint8_t interest) noexcept {
    return modify(fd, token, interest);
}

bool Reactor::modify(int fd, std::uint64_t token, std::uint8_t interest) noexcept {
    // kqueue tracks read and write filters independently, so a change is always
    // expressed as one ADD/DELETE per filter rather than a single mask.
    struct kevent ev[2];
    int n = 0;
    void* udata = reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
    EV_SET(&ev[n++], std::uintptr_t(fd), EVFILT_READ,
           (interest & kRead) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, udata);
    EV_SET(&ev[n++], std::uintptr_t(fd), EVFILT_WRITE,
           (interest & kWrite) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, udata);

    // EV_DELETE of a filter that was never added returns ENOENT; that is the
    // expected outcome of "disarm write interest" and is not an error.
    for (int i = 0; i < n; ++i) {
        if (::kevent(mux_.get(), &ev[i], 1, nullptr, 0, nullptr) != 0 && errno != ENOENT)
            return false;
    }
    return true;
}

bool Reactor::remove(int fd) noexcept { return modify(fd, 0, kNone); }

int Reactor::poll(Event* out, int max_events, int timeout_ms) noexcept {
    struct kevent evs[512];
    if (max_events > 512) max_events = 512;

    timespec ts;
    timespec* pts = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = long(timeout_ms % 1000) * 1'000'000;
        pts = &ts;
    }

    int n;
    do { n = ::kevent(mux_.get(), nullptr, 0, evs, max_events, pts); }
    while (n < 0 && errno == EINTR);
    if (n < 0) return -1;

    for (int i = 0; i < n; ++i) {
        Event& e = out[i];
        e = {};
        e.token    = std::uint64_t(reinterpret_cast<std::uintptr_t>(evs[i].udata));
        e.readable = evs[i].filter == EVFILT_READ;
        e.writable = evs[i].filter == EVFILT_WRITE;
        e.hangup   = (evs[i].flags & EV_EOF) != 0;
        e.error    = (evs[i].flags & EV_ERROR) != 0;
        if (e.token == kWakeToken) {
            char drain[256];
            while (::read(wake_read_.get(), drain, sizeof drain) > 0) {}
        }
    }
    return n;
}

bool Reactor::wake() noexcept {
    const char b = 1;
    return ::write(wake_write_.get(), &b, 1) == 1 || errno == EAGAIN;
}

#elif defined(GE_OS_LINUX)

namespace {
std::uint32_t to_epoll(std::uint8_t interest) noexcept {
    std::uint32_t m = 0;
    if (interest & kRead)  m |= EPOLLIN;
    if (interest & kWrite) m |= EPOLLOUT;
    return m | EPOLLRDHUP;
}
} // namespace

bool Reactor::add(int fd, std::uint64_t token, std::uint8_t interest) noexcept {
    if (use_uring_) {
        if (fd < 0 || std::size_t(fd) >= interest_.size()) return false;
        interest_[std::size_t(fd)] = interest;
        tokens_[std::size_t(fd)]   = token;
        return submit_poll(fd, token, interest);
    }
    epoll_event ev{};
    ev.events   = to_epoll(interest);
    ev.data.u64 = token;
    return ::epoll_ctl(mux_.get(), EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Reactor::modify(int fd, std::uint64_t token, std::uint8_t interest) noexcept {
    if (use_uring_) {
        // The next re-arm picks up the new interest. A poll already in flight
        // fires once with the old mask, which the caller handles anyway because
        // readiness is advisory -- it re-checks by attempting the I/O.
        if (fd < 0 || std::size_t(fd) >= interest_.size()) return false;
        interest_[std::size_t(fd)] = interest;
        tokens_[std::size_t(fd)]   = token;
        return true;
    }
    epoll_event ev{};
    ev.events   = to_epoll(interest);
    ev.data.u64 = token;
    return ::epoll_ctl(mux_.get(), EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool Reactor::remove(int fd) noexcept {
    if (use_uring_) {
        if (fd < 0 || std::size_t(fd) >= interest_.size()) return false;
        interest_[std::size_t(fd)] = kNone;      // stop re-arming; the in-flight
        return true;                             // poll dies with the fd
    }
    return ::epoll_ctl(mux_.get(), EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int Reactor::poll(Event* out, int max_events, int timeout_ms) noexcept {
    if (use_uring_) return poll_uring(out, max_events, timeout_ms);
    epoll_event evs[512];
    if (max_events > 512) max_events = 512;
    int n;
    do { n = ::epoll_wait(mux_.get(), evs, max_events, timeout_ms); }
    while (n < 0 && errno == EINTR);
    if (n < 0) return -1;

    for (int i = 0; i < n; ++i) {
        Event& e = out[i];
        e = {};
        e.token    = evs[i].data.u64;
        e.readable = (evs[i].events & EPOLLIN) != 0;
        e.writable = (evs[i].events & EPOLLOUT) != 0;
        e.hangup   = (evs[i].events & (EPOLLHUP | EPOLLRDHUP)) != 0;
        e.error    = (evs[i].events & EPOLLERR) != 0;
        if (e.token == kWakeToken) {
            std::uint64_t drain;
            while (::read(wake_read_.get(), &drain, sizeof drain) > 0) {}
        }
    }
    return n;
}

bool Reactor::wake() noexcept {
    const std::uint64_t v = 1;
    return ::write(wake_read_.get(), &v, sizeof v) == sizeof v || errno == EAGAIN;
}

#endif

} // namespace ge::net

// ---------------------------------------------------------------------------
// io_uring readiness backend.
//
// UNVALIDATED: written against the kernel ABI but never run on a machine with
// io_uring. It is off by default for that reason. docs/tutorial/14 explains how
// to validate it, and scripts/cloud/ provisions a VM to do so.
// ---------------------------------------------------------------------------
#if defined(GE_OS_LINUX)

namespace ge::net {
namespace {

int uring_setup(unsigned entries, io_uring_params* p) {
    return int(::syscall(__NR_io_uring_setup, entries, p));
}
int uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags,
                const void* arg = nullptr, std::size_t argsz = 0) {
    return int(::syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                         arg, argsz));
}

// min_complete is only honoured when GETEVENTS is set. Without it the kernel
// submits and returns immediately however large min_complete is -- which is the
// difference between an event loop that sleeps and one that spins a core.
#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS (1u << 0)
#endif
// EXT_ARG (5.11+) is what gives the wait a timeout without burning an SQE on
// IORING_OP_TIMEOUT. Without a timeout the wait is unbounded, and the periodic
// work the loop owes its caller never runs.
#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG (1u << 3)
#endif

struct getevents_arg {
    std::uint64_t sigmask;
    std::uint32_t sigmask_sz;
    std::uint32_t pad;
    std::uint64_t ts;
};
struct kernel_ts { std::int64_t tv_sec; std::int64_t tv_nsec; };

// The rings are written by one side and read by the other, so the indices need
// real acquire/release ordering -- exactly the same reasoning as the SPSC ring
// in part 5, except the producer here is the kernel.
inline unsigned load_acquire(const unsigned* p) {
    return std::atomic_ref<const unsigned>(*p).load(std::memory_order_acquire);
}
inline void store_release(unsigned* p, unsigned v) {
    std::atomic_ref<unsigned>(*p).store(v, std::memory_order_release);
}

constexpr std::size_t kMaxTrackedFd = 65536;

} // namespace

core::Status Reactor::open_uring() {
    io_uring_params params{};
    const int fd = uring_setup(4096, &params);
    if (fd < 0) return core::fail("io_uring_setup", std::strerror(errno));
    ring_fd_ = Fd(fd);

    // One mapping covers both rings when the kernel advertises SINGLE_MMAP,
    // which every kernel new enough to be worth targeting does.
    if (!(params.features & IORING_FEAT_SINGLE_MMAP))
        return core::fail("io_uring kernel too old (no SINGLE_MMAP)");

    ring_.sq_bytes = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    ring_.cq_bytes = params.cq_off.cqes  + params.cq_entries * sizeof(io_uring_cqe);
    const std::size_t bytes = std::max(ring_.sq_bytes, ring_.cq_bytes);

    void* base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (base == MAP_FAILED) return core::fail("mmap(SQ ring)", std::strerror(errno));
    ring_.sq_mmap = ring_.cq_mmap = base;

    auto* b = static_cast<unsigned char*>(base);
    ring_.sq_head  = reinterpret_cast<unsigned*>(b + params.sq_off.head);
    ring_.sq_tail  = reinterpret_cast<unsigned*>(b + params.sq_off.tail);
    ring_.sq_mask  = reinterpret_cast<unsigned*>(b + params.sq_off.ring_mask);
    ring_.sq_array = reinterpret_cast<unsigned*>(b + params.sq_off.array);
    ring_.cq_head  = reinterpret_cast<unsigned*>(b + params.cq_off.head);
    ring_.cq_tail  = reinterpret_cast<unsigned*>(b + params.cq_off.tail);
    ring_.cq_mask  = reinterpret_cast<unsigned*>(b + params.cq_off.ring_mask);
    ring_.cqes     = b + params.cq_off.cqes;
    ring_.sq_entries = params.sq_entries;

    ring_.sqe_bytes = params.sq_entries * sizeof(io_uring_sqe);
    ring_.sqe_mmap  = ::mmap(nullptr, ring_.sqe_bytes, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (ring_.sqe_mmap == MAP_FAILED) return core::fail("mmap(SQEs)", std::strerror(errno));
    ring_.sqes = ring_.sqe_mmap;

    interest_.assign(kMaxTrackedFd, 0);
    tokens_.assign(kMaxTrackedFd, 0);

    // add() dispatches on use_uring_, so it has to be true before the wakeup fd
    // is registered: on this path mux_ is never opened, and the epoll branch
    // would call epoll_ctl(-1, ...) and fail with EBADF. Cleared again on the
    // error paths below so an Auto fallback to epoll sees a consistent object.
    use_uring_ = true;

    wake_read_ = Fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake_read_.valid()) {
        use_uring_ = false;
        return core::fail("eventfd", std::strerror(errno));
    }
    if (!add(wake_read_.get(), kWakeToken, kRead)) {
        use_uring_ = false;
        // Not a syscall failure, so errno would be stale and misleading here.
        return core::fail("register wakeup", "io_uring submission queue full");
    }
    return core::ok();
}

bool Reactor::submit_poll(int fd, std::uint64_t token, std::uint8_t interest) noexcept {
    if (fd < 0 || std::size_t(fd) >= interest_.size() || interest == kNone) return true;

    const unsigned tail = *ring_.sq_tail;             // ours to read plainly
    const unsigned head = load_acquire(ring_.sq_head);
    if (tail - head >= ring_.sq_entries) return false;   // submission queue full

    const unsigned idx = tail & *ring_.sq_mask;
    auto* sqe = static_cast<io_uring_sqe*>(ring_.sqes) + idx;
    std::memset(sqe, 0, sizeof *sqe);
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd     = fd;
    sqe->len    = 0;
    unsigned events = 0;
    if (interest & kRead)  events |= POLLIN;
    if (interest & kWrite) events |= POLLOUT;
    sqe->poll32_events = events;
    // user_data carries the *fd*, not the caller's token. The caller's token is
    // already in tokens_[fd], so this direction is the one that needs to be
    // cheap: a completion must map back to an fd to re-arm single-shot poll.
    (void)token;
    // Two-step cast on purpose: user_data is __u64 (unsigned long long) while
    // uint64_t is unsigned long here, and GCC's -Wsign-conversion fires on the
    // direct assignment. Widening from unsigned keeps it silent and exact.
    sqe->user_data     = static_cast<unsigned long long>(static_cast<unsigned>(fd));

    ring_.sq_array[idx] = idx;
    store_release(ring_.sq_tail, tail + 1);
    ++pending_submits_;
    return true;
}

void Reactor::rearm(int fd) noexcept {
    if (fd < 0 || std::size_t(fd) >= interest_.size()) return;
    const std::uint8_t want = interest_[std::size_t(fd)];
    if (want != kNone) submit_poll(fd, tokens_[std::size_t(fd)], want);
}

int Reactor::poll_uring(Event* out, int max_events, int timeout_ms) noexcept {
    // Submit everything queued and wait in one syscall. This is the win over
    // epoll: interest changes and the wait are not separate transitions.
    const unsigned min_complete = timeout_ms == 0 ? 0u : 1u;
    if (pending_submits_ > 0 || min_complete > 0) {
        int rc;
        if (min_complete > 0) {
            kernel_ts ts{ timeout_ms / 1000, std::int64_t(timeout_ms % 1000) * 1000000 };
            getevents_arg a{};
            a.ts = reinterpret_cast<std::uint64_t>(&ts);
            rc = uring_enter(ring_fd_.get(), pending_submits_, min_complete,
                             IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &a, sizeof a);
        } else {
            rc = uring_enter(ring_fd_.get(), pending_submits_, 0, 0);
        }
        if (rc < 0 && errno != EINTR && errno != EAGAIN && errno != ETIME) return -1;
        pending_submits_ = 0;
    }

    unsigned head = *ring_.cq_head;
    const unsigned tail = load_acquire(ring_.cq_tail);
    int n = 0;
    auto* cqes = static_cast<io_uring_cqe*>(ring_.cqes);
    while (head != tail && n < max_events) {
        const io_uring_cqe& cqe = cqes[head & *ring_.cq_mask];
        const std::uint64_t ud = cqe.user_data;
        const int fd = (ud < interest_.size()) ? int(ud) : -1;
        Event& e = out[n];
        e = {};
        e.token = (fd >= 0) ? tokens_[std::size_t(fd)] : ud;
        if (cqe.res >= 0) {
            const unsigned mask = unsigned(cqe.res);
            e.readable = (mask & POLLIN)  != 0;
            e.writable = (mask & POLLOUT) != 0;
            e.hangup   = (mask & (POLLHUP | POLLRDHUP)) != 0;
            e.error    = (mask & POLLERR) != 0;
        } else {
            e.error = true;
        }
        if (e.token == kWakeToken) {
            std::uint64_t drain;
            while (::read(wake_read_.get(), &drain, sizeof drain) > 0) {}
        }
        // Single-shot poll is consumed by firing, so an fd that reported has to
        // be registered again. This used to be a linear scan over all 65,536
        // tracked slots per completion, looking the token back up to find its
        // fd -- more work than the syscall io_uring exists to remove.
        // Multishot (IORING_POLL_ADD_MULTI, 5.13+) would drop the re-arm itself.
        if (fd >= 0) rearm(fd);
        ++head;
        ++n;
    }
    store_release(ring_.cq_head, head);
    return n;
}

} // namespace ge::net

#endif  // GE_OS_LINUX
