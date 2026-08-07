#pragma once
// Zero-copy framing.
//
// Decoding a message produces a *view*: a pointer into the socket's receive
// buffer plus a compile-time-checked type. Nothing is copied, nothing is
// allocated, and the variable-length tail of a StateDelta is handed to the
// caller as a std::span aliasing the same buffer.
//
// The safety of that cast rests on three invariants, all checked here rather
// than assumed:
//   1. the buffer base is 8-byte aligned (asserted at construction),
//   2. every frame length is a multiple of 8 (rejected at parse time), so the
//      next frame is aligned too,
//   3. the declared payload length matches what the type requires (rejected at
//      parse time), so a short read can never be cast to a longer struct.
// A peer that violates any of them gets a ParseError and a closed connection;
// it never gets to read past the end of our buffer.
#include <algorithm>
#include <cstring>
#include <utility>
#include <span>
#include <type_traits>

#include "ge/core/assert.hpp"
#include "ge/core/bits.hpp"
#include "ge/proto/wire.hpp"

namespace ge::proto {

enum class ParseError : std::uint8_t {
    Ok = 0,
    Incomplete,   // not an error: more bytes needed
    BadMagic,     // stream desync or a non-peer talking to us
    BadType,
    BadLength,    // not 8-aligned, or wrong size for the declared type
    TooLarge,
};

constexpr const char* parse_error_name(ParseError e) noexcept {
    switch (e) {
        case ParseError::Ok:         return "ok";
        case ParseError::Incomplete: return "incomplete";
        case ParseError::BadMagic:   return "bad-magic";
        case ParseError::BadType:    return "bad-type";
        case ParseError::BadLength:  return "bad-length";
        case ParseError::TooLarge:   return "too-large";
    }
    return "?";
}

constexpr std::size_t frame_size(std::uint16_t payload_len) noexcept {
    return sizeof(FrameHeader) + payload_len;
}

// A decoded frame that still lives in the receive buffer.
class FrameView {
public:
    FrameView() = default;
    FrameView(const std::byte* p, std::uint16_t payload_len) noexcept
        : base_(p), payload_len_(payload_len) {}

    const FrameHeader& header() const noexcept {
        return *reinterpret_cast<const FrameHeader*>(base_);
    }
    MsgType       type()        const noexcept { return MsgType(header().type); }
    std::uint32_t session()     const noexcept { return header().session; }
    std::uint16_t payload_len() const noexcept { return payload_len_; }
    std::size_t   total_bytes() const noexcept { return sizeof(FrameHeader) + payload_len_; }

    // Raw bytes of the whole frame: what a proxy forwards verbatim.
    std::span<const std::byte> bytes() const noexcept { return {base_, total_bytes()}; }

    // Typed view. Returns nullptr unless the declared type matches T *and* the
    // payload is long enough -- a caller cannot bypass the check by guessing.
    template <typename T>
    const T* as() const noexcept {
        if (type() != MsgTraits<T>::type)      return nullptr;
        if (payload_len_ < sizeof(T))          return nullptr;
        const std::byte* p = base_ + sizeof(FrameHeader);
        GE_ASSERT(reinterpret_cast<std::uintptr_t>(p) % kFrameAlign == 0);
        // The parser rejected any frame whose length was not a multiple of 8, so
        // this pointer is 8-aligned by construction. Telling the optimiser lets
        // it assume aligned loads without a runtime check. It is an assumption,
        // not a check -- the check is the one the parser already did.
        [[assume(reinterpret_cast<std::uintptr_t>(p) % kFrameAlign == 0)]];
        return reinterpret_cast<const T*>(p);
    }

    // Bytes after the fixed header, for payloads whose tail is not a flat
    // array. This is what a nested reader walks.
    std::span<const std::byte> tail_bytes(std::size_t header_size) const noexcept {
        if (payload_len_ < header_size) return {};
        return {base_ + sizeof(FrameHeader) + header_size, payload_len_ - header_size};
    }

    // Variable-length tail, aliasing the receive buffer. No copy, no allocation.
    template <typename T>
    std::span<const typename MsgTraits<T>::element> tail() const noexcept {
        using E = typename MsgTraits<T>::element;
        static_assert(MsgTraits<T>::variable, "only variable-length messages have a tail");
        static_assert(!MsgTraits<T>::nested,
                      "this payload's tail is a sequence of variable-sized records, not a flat "
                      "array; there is no constant stride to form a span over. Walk it with the "
                      "dedicated reader (e.g. SnakeDeltaView) instead.");
        const T* h = as<T>();
        if (!h) return {};
        // Two independent bounds, and the smaller wins:
        //   * what the buffer physically holds -- validated at parse time, and
        //     the only one that keeps the span inside our memory;
        //   * what the sender declared -- attacker-controlled, so it may shrink
        //     the view but can never extend it past the first bound.
        // Using only the declared count would be a buffer overrun; using only
        // the physical size would silently hand the caller trailing bytes the
        // sender never meant as elements.
        const std::size_t capacity = (payload_len_ - sizeof(T)) / sizeof(E);
        std::size_t n = capacity;
        if constexpr (requires { h->count; })
            n = std::min(capacity, std::size_t(h->count));
        const auto* first = reinterpret_cast<const E*>(base_ + sizeof(FrameHeader) + sizeof(T));
        return {first, n};
    }

private:
    const std::byte* base_        = nullptr;
    std::uint16_t    payload_len_ = 0;
};

// Iterates complete frames out of a receive buffer, leaving any trailing
// partial frame for the next read.
class FrameReader {
public:
    explicit FrameReader(std::span<const std::byte> buf) noexcept : buf_(buf) {
        GE_ASSERT(buf.empty() ||
                  reinterpret_cast<std::uintptr_t>(buf.data()) % kFrameAlign == 0);
    }

    ParseError next(FrameView& out) noexcept {
        const std::size_t avail = buf_.size() - off_;
        if (avail < sizeof(FrameHeader)) return ParseError::Incomplete;

        const std::byte* p = buf_.data() + off_;
        FrameHeader h;
        std::memcpy(&h, p, sizeof h);                 // header copy is 16B, kept off the alias path

        if (GE_UNLIKELY(h.magic != kMagic))                     return ParseError::BadMagic;
        if (GE_UNLIKELY(h.payload_len % kFrameAlign != 0))      return ParseError::BadLength;
        if (GE_UNLIKELY(h.payload_len > kMaxPayload))           return ParseError::TooLarge;
        if (GE_UNLIKELY(h.type == 0 || h.type >= std::uint8_t(MsgType::Count)))
                                                                return ParseError::BadType;
        const std::size_t total = sizeof(FrameHeader) + h.payload_len;
        if (avail < total) return ParseError::Incomplete;
        if (GE_UNLIKELY(!payload_fits(MsgType(h.type), h.payload_len)))
                                                                return ParseError::BadLength;
        out  = FrameView(p, h.payload_len);
        off_ += total;
        return ParseError::Ok;
    }

    std::size_t consumed()  const noexcept { return off_; }
    std::size_t remaining() const noexcept { return buf_.size() - off_; }

private:
    // Minimum payload each type requires. Enforcing this at the boundary is
    // what lets as<T>() be a pointer cast instead of a bounds check per field.
    static bool payload_fits(MsgType t, std::uint16_t len) noexcept {
        switch (t) {
            case MsgType::ClientHello:   return len >= sizeof(ClientHello);
            case MsgType::ServerWelcome: return len >= sizeof(ServerWelcome);
            case MsgType::Ping:          return len >= sizeof(Ping);
            case MsgType::Pong:          return len >= sizeof(Pong);
            case MsgType::MoveCommand:   return len >= sizeof(MoveCommand);
            case MsgType::SnakeDelta:    return len >= sizeof(SnakeDeltaHeader);
            case MsgType::Disconnect:    return len >= sizeof(Disconnect);
            case MsgType::HealthProbe:   return len >= sizeof(HealthProbe);
            case MsgType::HealthReply:   return len >= sizeof(HealthReply);
            case MsgType::ShooterInput:  return len >= sizeof(ShooterInput);
            case MsgType::ShooterDelta:  return len >= sizeof(ShooterDeltaHeader);
            case MsgType::Invalid:
            case MsgType::Count:         break;
        }
        return false;
    }

    std::span<const std::byte> buf_;
    std::size_t                off_ = 0;
};

// Type-safe dispatch. The switch is over a dense enum, so the compiler emits a
// jump table; the handler is called with the concrete type, so there is no
// virtual call and no downcast in user code.
template <typename H>
void visit(const FrameView& f, H&& h) {
    auto call = [&](auto* tag) {
        using T = std::remove_pointer_t<decltype(tag)>;
        const T* m = f.as<T>();
        if (!m) return;
        if constexpr (MsgTraits<T>::nested)
            h(*m, f);                       // nested tails need the frame, not a span
        else if constexpr (MsgTraits<T>::variable)
            h(*m, f.tail<T>());
        else
            h(*m);
    };
    switch (f.type()) {
        case MsgType::ClientHello:   call(static_cast<ClientHello*>(nullptr));      break;
        case MsgType::ServerWelcome: call(static_cast<ServerWelcome*>(nullptr));    break;
        case MsgType::Ping:          call(static_cast<Ping*>(nullptr));             break;
        case MsgType::Pong:          call(static_cast<Pong*>(nullptr));             break;
        case MsgType::MoveCommand:   call(static_cast<MoveCommand*>(nullptr));      break;
        case MsgType::SnakeDelta:    call(static_cast<SnakeDeltaHeader*>(nullptr)); break;
        case MsgType::Disconnect:    call(static_cast<Disconnect*>(nullptr));       break;
        case MsgType::HealthProbe:   call(static_cast<HealthProbe*>(nullptr));      break;
        case MsgType::HealthReply:   call(static_cast<HealthReply*>(nullptr));      break;
        case MsgType::ShooterInput:  call(static_cast<ShooterInput*>(nullptr));     break;
        case MsgType::ShooterDelta:  call(static_cast<ShooterDeltaHeader*>(nullptr)); break;
        case MsgType::Invalid:
        case MsgType::Count:         break;
    }
}

// ---------------------------------------------------------- nested tails
// A SnakeDelta's tail is a run of (SnakeRecord, Point2[segments]) pairs, so it
// cannot be a span. This walks it, and -- because this is untrusted input -- it
// re-derives every bound from the frame length rather than believing the
// `segments` count in the record. A record claiming 60000 segments yields the
// end of iteration, not a read past the buffer.
class SnakeDeltaView {
public:
    explicit SnakeDeltaView(const FrameView& f) noexcept
        : header_(f.as<SnakeDeltaHeader>()),
          rest_(header_ ? f.tail_bytes(sizeof(SnakeDeltaHeader)) : std::span<const std::byte>{}) {}

    bool valid() const noexcept { return header_ != nullptr; }
    const SnakeDeltaHeader& header() const noexcept { return *header_; }

    // Advances to the next snake. Returns false at the end, or on any record
    // that does not fit -- a truncated or hostile tail simply ends iteration.
    bool next(const SnakeRecord*& rec, std::span<const Point2>& points) noexcept {
        if (!header_ || emitted_ >= header_->count) return false;
        if (off_ + sizeof(SnakeRecord) > rest_.size()) return false;

        const auto* r = reinterpret_cast<const SnakeRecord*>(rest_.data() + off_);
        const std::size_t bytes = std::size_t(r->segments) * sizeof(Point2);
        if (off_ + sizeof(SnakeRecord) + bytes > rest_.size()) return false;

        rec    = r;
        points = {reinterpret_cast<const Point2*>(rest_.data() + off_ + sizeof(SnakeRecord)),
                  r->segments};
        off_ += sizeof(SnakeRecord) + bytes;
        ++emitted_;
        return true;
    }

private:
    const SnakeDeltaHeader*    header_;
    std::span<const std::byte> rest_;
    std::size_t                off_     = 0;
    std::uint16_t              emitted_ = 0;
};

// Builds a SnakeDelta straight into destination memory: the simulation formats
// frames directly into the outbound ring, so there is no staging buffer between
// computing a position and handing the bytes to write(2).
class SnakeDeltaWriter {
public:
    SnakeDeltaWriter(std::span<std::byte> out, std::uint32_t session, std::uint32_t seq) noexcept
        : out_(out), session_(session), seq_(seq) {
        GE_ASSERT(out.empty() || reinterpret_cast<std::uintptr_t>(out.data()) % kFrameAlign == 0);
        off_ = sizeof(FrameHeader) + sizeof(SnakeDeltaHeader);
        ok_  = out.size() >= off_;
    }

    bool valid() const noexcept { return ok_; }

    // Reserves room for one snake and returns the points to fill in place.
    // Returns an empty span when the frame is full, which the caller treats as
    // "stop adding snakes", never as an error.
    std::span<Point2> begin_snake(std::uint16_t max_segments) noexcept {
        if (!ok_) return {};
        const std::size_t need = sizeof(SnakeRecord) + std::size_t(max_segments) * sizeof(Point2);
        if (off_ + need > out_.size()) return {};
        pending_ = off_;
        return {reinterpret_cast<Point2*>(out_.data() + off_ + sizeof(SnakeRecord)), max_segments};
    }

    void commit_snake(const SnakeRecord& rec, std::uint16_t segments) noexcept {
        if (!ok_) return;
        SnakeRecord r = rec;
        r.segments = segments;
        std::memcpy(out_.data() + pending_, &r, sizeof r);
        off_ = pending_ + sizeof(SnakeRecord) + std::size_t(segments) * sizeof(Point2);
        ++count_;
    }

    std::size_t finish(const SnakeDeltaHeader& hdr) noexcept {
        if (!ok_) return 0;
        SnakeDeltaHeader h = hdr;
        h.count = count_;
        const auto payload = std::uint16_t(off_ - sizeof(FrameHeader));
        const FrameHeader fh{kMagic, payload, std::to_underlying(MsgType::SnakeDelta),
                             std::to_underlying(FrameFlags::ServerSent), session_, seq_};
        std::memcpy(out_.data(), &fh, sizeof fh);
        std::memcpy(out_.data() + sizeof fh, &h, sizeof h);
        return off_;
    }

    std::uint16_t count() const noexcept { return count_; }

private:
    std::span<std::byte> out_;
    std::uint32_t        session_;
    std::uint32_t        seq_;
    std::size_t          off_     = 0;
    std::size_t          pending_ = 0;
    std::uint16_t        count_   = 0;
    bool                 ok_      = false;
};

// A ShooterDelta's tail is two flat arrays back to back: `count` PlayerRecords
// then `pickup_count` PickupRecords. Each array has a constant stride, but the
// tail as a whole does not, which is why ShooterDeltaHeader is marked nested
// and tail<T>() refuses to form one span over it. The split happens here,
// once, instead of at every call site getting the offset arithmetic right.
//
// Same two-bound rule tail<T>() uses, for the same reason: the physical
// payload length is the only bound that keeps a span inside our own memory,
// and the counts the sender declared may shrink a view but can never extend
// it. A header claiming 60000 players yields the records that actually fit,
// not a read past the buffer.
class ShooterDeltaView {
public:
    explicit ShooterDeltaView(const FrameView& f) noexcept
        : header_(f.as<ShooterDeltaHeader>()) {
        if (!header_) return;
        const auto rest = f.tail_bytes(sizeof(ShooterDeltaHeader));
        if (rest.empty()) return;

        const std::size_t np = std::min(rest.size() / sizeof(PlayerRecord),
                                        std::size_t(header_->count));
        players_ = {reinterpret_cast<const PlayerRecord*>(rest.data()), np};

        // The pickup array starts where the player array ended, so its offset
        // depends on the *clamped* count rather than the declared one. Using
        // the declared count here would reintroduce exactly the overrun the
        // clamp above prevents.
        const std::size_t used = np * sizeof(PlayerRecord);
        const std::size_t nk = std::min((rest.size() - used) / sizeof(PickupRecord),
                                        std::size_t(header_->pickup_count));
        if (nk)
            pickups_ = {reinterpret_cast<const PickupRecord*>(rest.data() + used), nk};
    }

    bool valid() const noexcept { return header_ != nullptr; }
    const ShooterDeltaHeader&     header()  const noexcept { return *header_; }
    std::span<const PlayerRecord> players() const noexcept { return players_; }
    std::span<const PickupRecord> pickups() const noexcept { return pickups_; }

private:
    const ShooterDeltaHeader*     header_ = nullptr;
    std::span<const PlayerRecord> players_;
    std::span<const PickupRecord> pickups_;
};

// ------------------------------------------------------------------ encoding
// Encoding writes straight into destination memory the caller already owns --
// a socket send buffer or a shard-to-I/O ring slot. There is no intermediate
// buffer and therefore no copy between "serialise" and "send".

template <typename T>
std::size_t encode(std::span<std::byte> out, const T& body,
                   std::uint32_t session, std::uint32_t seq,
                   std::uint8_t flags = 0) noexcept {
    static_assert(!MsgTraits<T>::variable, "use encode_state_delta for variable-length messages");
    const std::size_t total = sizeof(FrameHeader) + sizeof(T);
    if (GE_UNLIKELY(out.size() < total)) return 0;
    GE_ASSERT(reinterpret_cast<std::uintptr_t>(out.data()) % kFrameAlign == 0);

    const FrameHeader h{kMagic, std::uint16_t(sizeof(T)),
                        std::to_underlying(MsgTraits<T>::type), flags, session, seq};
    std::memcpy(out.data(), &h, sizeof h);
    std::memcpy(out.data() + sizeof h, &body, sizeof(T));
    return total;
}

// Encode a variable-length message whose tail is a FLAT array of fixed-size
// elements.
//
// Unlike SnakeDeltaWriter this needs no incremental builder: a flat tail has a
// constant stride, so the element count is known before anything is written and
// header plus tail go down in one pass. The receiver reads it back with
// FrameView::tail<E>(), which is exactly the case tail<E>() exists for.
template <typename H, typename E>
std::size_t encode_flat(std::span<std::byte> out, const H& head,
                        std::span<const E> tail, std::uint32_t session,
                        std::uint32_t seq, std::uint8_t flags = 0) noexcept {
    static_assert(MsgTraits<H>::variable, "encode_flat is for variable-length messages");
    static_assert(!MsgTraits<H>::nested,  "nested tails need a dedicated writer");
    static_assert(std::is_same_v<typename MsgTraits<H>::element, E>,
                  "tail element type does not match the message's declared element");
    const std::size_t payload = sizeof(H) + tail.size() * sizeof(E);
    const std::size_t total   = sizeof(FrameHeader) + payload;
    if (GE_UNLIKELY(out.size() < total || payload > kMaxPayload)) return 0;
    GE_ASSERT(reinterpret_cast<std::uintptr_t>(out.data()) % kFrameAlign == 0);
    GE_ASSERT(payload % kFrameAlign == 0);

    const FrameHeader h{kMagic, std::uint16_t(payload),
                        std::to_underlying(MsgTraits<H>::type), flags, session, seq};
    std::memcpy(out.data(), &h, sizeof h);
    std::memcpy(out.data() + sizeof h, &head, sizeof(H));
    if (!tail.empty())
        std::memcpy(out.data() + sizeof h + sizeof(H), tail.data(),
                    tail.size() * sizeof(E));
    return total;
}

// Encode a message whose tail is TWO flat arrays back to back -- the shape a
// ShooterDelta took on once it had to carry powerups as well as players.
//
// This is not the nested case SnakeDeltaWriter handles. There, record sizes
// vary and the tail can only be built incrementally. Here both strides are
// constant and both counts are known before anything is written, so the frame
// still goes down in one pass; all that is needed is a second memcpy and a
// header field saying where the boundary is. Keeping it a separate function
// rather than generalising encode_flat means the single-array case keeps its
// stricter static_asserts.
template <typename H, typename E1, typename E2>
std::size_t encode_flat2(std::span<std::byte> out, const H& head,
                         std::span<const E1> a, std::span<const E2> b,
                         std::uint32_t session, std::uint32_t seq,
                         std::uint8_t flags = 0) noexcept {
    static_assert(MsgTraits<H>::variable, "encode_flat2 is for variable-length messages");
    static_assert(std::is_same_v<typename MsgTraits<H>::element, E1>,
                  "first tail array does not match the message's declared element");
    static_assert(std::is_trivially_copyable_v<E2>);
    // Each array must be 8-aligned on its own. If the first were not, the
    // second would start misaligned no matter how the frame was laid out.
    static_assert(sizeof(E1) % kFrameAlign == 0, "first tail element breaks frame alignment");
    static_assert(sizeof(E2) % kFrameAlign == 0, "second tail element breaks frame alignment");

    const std::size_t payload = sizeof(H) + a.size() * sizeof(E1) + b.size() * sizeof(E2);
    const std::size_t total   = sizeof(FrameHeader) + payload;
    if (GE_UNLIKELY(out.size() < total || payload > kMaxPayload)) return 0;
    GE_ASSERT(reinterpret_cast<std::uintptr_t>(out.data()) % kFrameAlign == 0);
    GE_ASSERT(payload % kFrameAlign == 0);

    const FrameHeader h{kMagic, std::uint16_t(payload),
                        std::to_underlying(MsgTraits<H>::type), flags, session, seq};
    std::byte* p = out.data();
    std::memcpy(p, &h, sizeof h);                 p += sizeof h;
    std::memcpy(p, &head, sizeof(H));             p += sizeof(H);
    if (!a.empty()) { std::memcpy(p, a.data(), a.size() * sizeof(E1)); p += a.size() * sizeof(E1); }
    if (!b.empty())   std::memcpy(p, b.data(), b.size() * sizeof(E2));
    return total;
}

} // namespace ge::proto
