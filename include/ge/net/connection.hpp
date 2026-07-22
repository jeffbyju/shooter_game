#pragma once
// Per-connection buffering.
//
// Receive side: one contiguous, 8-byte-aligned buffer that the protocol parser
// reads *in place*. Compaction moves only the trailing partial frame, and only
// by a multiple of 8, which preserves the alignment invariant the zero-copy
// cast depends on.
//
// Send side: a byte ring drained with writev. The interesting part is what
// happens when a client stops reading. The kernel send buffer fills, then this
// ring fills, and at that point the server has three options: block (which
// would stall every other client on the thread), grow the buffer without bound
// (which is how a server OOMs from one bad client), or shed. It sheds -- the
// connection is evicted and counted. Bounded memory per connection is a
// property of the type, not of client behaviour.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "ge/core/assert.hpp"
#include "ge/core/bits.hpp"
#include "ge/core/platform.hpp"
#include "ge/net/socket.hpp"
#include "ge/proto/wire.hpp"

namespace ge::net {

class Connection {
public:
    void init(int fd, std::uint32_t id,
              std::byte* rbuf, std::uint32_t rcap,
              std::byte* wbuf, std::uint32_t wcap) noexcept {
        GE_ASSERT(reinterpret_cast<std::uintptr_t>(rbuf) % proto::kFrameAlign == 0);
        // whead_/wtail_ are free-running uint32 counters wrapped with `% wcap_`;
        // that identity only holds across the 2^32 rollover if wcap_ is a power
        // of two, so the requirement is checked rather than documented.
        GE_CHECK(core::is_pow2(wcap));
        fd_ = fd; id_ = id;
        rbuf_ = rbuf; rcap_ = rcap; rlen_ = 0;
        wbuf_ = wbuf; wcap_ = wcap; whead_ = wtail_ = 0;
        session_ = 0; shard_ = 0; out_seq_ = 0; in_seq_ = 0;
        handshaked_ = false; closing_ = false;
        bytes_in_ = bytes_out_ = 0;
    }

    int           fd()      const noexcept { return fd_; }
    std::uint32_t id()      const noexcept { return id_; }
    std::uint32_t session() const noexcept { return session_; }
    std::uint16_t shard()   const noexcept { return shard_; }
    bool          handshaked() const noexcept { return handshaked_; }
    bool          closing()    const noexcept { return closing_; }

    void set_session(std::uint32_t s, std::uint16_t shard) noexcept {
        session_ = s; shard_ = shard; handshaked_ = true;
    }
    void mark_closing() noexcept { closing_ = true; }

    // ---- receive ----------------------------------------------------------
    IoOutcome fill() noexcept {
        if (rlen_ >= rcap_) return {IoResult::WouldBlock, 0, 0};   // parser must drain first
        IoOutcome r = read_some(fd_, rbuf_ + rlen_, rcap_ - rlen_);
        if (r.result == IoResult::Ok) {
            rlen_ += std::uint32_t(r.bytes);
            bytes_in_ += std::uint64_t(r.bytes);
        }
        return r;
    }

    std::span<const std::byte> readable() const noexcept { return {rbuf_, rlen_}; }
    std::uint32_t read_space() const noexcept { return rcap_ - rlen_; }

    // `n` is always a whole number of frames, hence a multiple of 8, so the new
    // base stays aligned and the cast in the codec stays defined.
    //
    // The bound is a GE_CHECK rather than a GE_ASSERT, so it survives into
    // release builds. Consuming more than is buffered underflows rlen_ to ~4e9
    // and turns the memmove below into a multi-gigabyte read off the end of the
    // buffer -- a silent heap corruption in the best case and a SIGSEGV in the
    // typical one. Aborting on a broken invariant is strictly better than
    // continuing to run with a corrupted process, and it names the bug instead
    // of leaving a stack trace inside memmove.
    void consume(std::size_t n) noexcept {
        GE_CHECK(n <= rlen_ && n % proto::kFrameAlign == 0);
        rlen_ -= std::uint32_t(n);
        if (rlen_ && n) std::memmove(rbuf_, rbuf_ + n, rlen_);
    }
    bool read_buffer_full() const noexcept { return rlen_ >= rcap_; }

    // ---- send -------------------------------------------------------------
    bool has_pending() const noexcept { return whead_ != wtail_; }
    std::uint32_t pending_bytes() const noexcept { return whead_ - wtail_; }
    std::uint32_t send_free() const noexcept { return wcap_ - pending_bytes(); }

    // Copy into the outbound ring. Returns false when the client is too far
    // behind: the caller evicts rather than growing the buffer.
    bool queue(const void* data, std::uint32_t n) noexcept;

    // The zero-copy send: hand bytes that live somewhere else (a shard's output
    // ring) straight to write(2) when nothing is queued ahead of them. Only a
    // partial write costs a copy, and only of the unsent remainder.
    bool send_now(const void* data, std::uint32_t n, IoOutcome& out) noexcept;

    IoOutcome flush() noexcept;

    // ---- accounting -------------------------------------------------------
    std::uint32_t next_out_seq() noexcept { return out_seq_++; }
    std::uint32_t in_seq()  const noexcept { return in_seq_; }
    void set_in_seq(std::uint32_t s) noexcept { in_seq_ = s; }
    std::uint64_t bytes_in()  const noexcept { return bytes_in_; }
    std::uint64_t bytes_out() const noexcept { return bytes_out_; }
    std::int64_t  last_active_ns() const noexcept { return last_active_ns_; }
    void touch(std::int64_t now) noexcept { last_active_ns_ = now; }

private:
    int           fd_   = -1;
    std::uint32_t id_   = 0;

    std::byte*    rbuf_ = nullptr;
    std::uint32_t rcap_ = 0;
    std::uint32_t rlen_ = 0;

    std::byte*    wbuf_  = nullptr;
    std::uint32_t wcap_  = 0;
    std::uint32_t whead_ = 0;   // monotonic; difference is the queue depth
    std::uint32_t wtail_ = 0;

    std::uint32_t session_ = 0;
    std::uint16_t shard_   = 0;
    std::uint32_t out_seq_ = 0;
    std::uint32_t in_seq_  = 0;
    bool          handshaked_ = false;
    bool          closing_    = false;

    std::uint64_t bytes_in_  = 0;
    std::uint64_t bytes_out_ = 0;
    std::int64_t  last_active_ns_ = 0;
};

} // namespace ge::net
