#include "ge/net/connection.hpp"

#include <algorithm>
#include <sys/uio.h>

namespace ge::net {

bool Connection::queue(const void* data, std::uint32_t n) noexcept {
    if (GE_UNLIKELY(n > send_free())) return false;    // caller evicts; we never grow
    const auto* src = static_cast<const std::byte*>(data);
    const std::uint32_t off   = whead_ % wcap_;
    const std::uint32_t first = std::min(n, wcap_ - off);
    std::memcpy(wbuf_ + off, src, first);
    if (n > first) std::memcpy(wbuf_, src + first, n - first);
    whead_ += n;
    return true;
}

bool Connection::send_now(const void* data, std::uint32_t n, IoOutcome& out) noexcept {
    if (has_pending()) {                                // preserve ordering
        out = {IoResult::WouldBlock, 0, 0};
        return queue(data, n);
    }
    out = write_some(fd_, data, n);
    if (out.result == IoResult::Ok) {
        bytes_out_ += std::uint64_t(out.bytes);
        const auto wrote = std::uint32_t(out.bytes);
        if (wrote == n) return true;                    // the common case: zero copies
        return queue(static_cast<const std::byte*>(data) + wrote, n - wrote);
    }
    if (out.result == IoResult::WouldBlock) return queue(data, n);
    return false;                                       // Closed/Error: caller tears down
}

IoOutcome Connection::flush() noexcept {
    if (!has_pending()) return {IoResult::Ok, 0, 0};

    // At most two segments, because the ring wraps at most once between tail
    // and head. One writev beats two writes: half the syscalls, and the kernel
    // sees a single contiguous logical buffer for TSO/coalescing purposes.
    const std::uint32_t total = pending_bytes();
    const std::uint32_t off   = wtail_ % wcap_;
    const std::uint32_t first = std::min(total, wcap_ - off);

    iovec iov[2];
    int iovcnt = 0;
    iov[iovcnt++] = {wbuf_ + off, first};
    if (total > first) iov[iovcnt++] = {wbuf_, total - first};

    IoOutcome r = (iovcnt == 1) ? write_some(fd_, iov[0].iov_base, iov[0].iov_len)
                                : writev_some(fd_, iov, iovcnt);
    if (r.result == IoResult::Ok) {
        wtail_ += std::uint32_t(r.bytes);
        bytes_out_ += std::uint64_t(r.bytes);
    }
    return r;
}

} // namespace ge::net
