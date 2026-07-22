#pragma once
// On-the-wire message definitions.
//
// Design rules, all enforced at compile time below:
//   * Every message is a fixed-layout, trivially copyable POD with an explicit
//     size and 8-byte alignment. No serialisation step exists: the struct is
//     the wire format, so a receiver reads it by pointing at the socket buffer.
//   * Every frame -- header plus payload -- is a multiple of 8 bytes. The
//     parser rejects anything else, which keeps the next frame in a stream
//     8-byte aligned and makes the zero-copy cast well-defined rather than
//     lucky.
//   * The header alone carries everything the gateway needs to route, so a
//     proxy hop never inspects, copies, or re-encodes a payload.
//   * Little-endian is the wire order; both supported targets are LE and the
//     static_assert below fails loudly on anything else rather than silently
//     shipping mojibake.
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "ge/core/platform.hpp"

namespace ge::proto {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "wire format is little-endian; a BE port needs byte-swapping accessors");

inline constexpr std::uint32_t kMagic           = 0x47454E31u;  // "GEN1"
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t   kFrameAlign      = 8;
inline constexpr std::size_t   kMaxPayload      = 32768;

enum class MsgType : std::uint8_t {
    Invalid = 0,
    ClientHello,        // C -> S  join request
    ServerWelcome,      // S -> C  session accepted
    Ping,               // C -> S  echoed on the I/O thread, never enters the sim
    Pong,               // S -> C
    MoveCommand,        // C -> S  input for the next tick
    SnakeDelta,         // S -> C  variable length: header + (record + points)[]
    Disconnect,         // either
    HealthProbe,        // gateway -> backend
    HealthReply,        // backend -> gateway
    Count
};

enum class FrameFlags : std::uint8_t {
    None       = 0,
    Reliable   = 1u << 0,
    Fragment   = 1u << 1,
    ServerSent = 1u << 2,
};
// std::to_underlying says what the cast is for. `std::uint8_t(a)` would also
// compile if FrameFlags were changed to a wider type, silently truncating;
// to_underlying tracks the enum's actual underlying type.
constexpr std::uint8_t operator|(FrameFlags a, FrameFlags b) noexcept {
    return std::uint8_t(std::to_underlying(a) | std::to_underlying(b));
}

// 16 bytes. `session` is the routing key: the gateway hashes it and forwards
// without ever looking past byte 16.
struct FrameHeader {
    std::uint32_t magic;
    std::uint16_t payload_len;   // bytes after this header; must be % 8 == 0
    std::uint8_t  type;          // MsgType
    std::uint8_t  flags;         // FrameFlags bitset
    std::uint32_t session;       // routing key, 0 before the handshake completes
    std::uint32_t seq;           // per-direction sequence, for loss/reorder accounting
};
static_assert(sizeof(FrameHeader) == 16 && alignof(FrameHeader) == 4);
static_assert(std::is_trivially_copyable_v<FrameHeader>);

// ---------------------------------------------------------------- payloads
struct ClientHello {
    std::uint64_t nonce;
    std::uint32_t protocol;
    std::uint32_t region_hint;
};

// Everything a client needs to make sense of what follows, so that none of it
// has to be configured out of band. A client told only "connect to this port"
// cannot render a world whose extent it does not know, and cannot explain to its
// user why it can see nobody else -- both of which are properties of the server,
// so the server states them.
struct ServerWelcome {
    std::uint64_t session_key;
    // The session id, not the simulation's entity id. The I/O thread cannot
    // know the latter here: Join is queued to the shard and the entity does not
    // exist until the next tick. A client identifies its own entity from the
    // kSnakeSelf flag on the first SnakeDelta instead.
    std::uint32_t session_id;
    float         world_size;    // the world is [0, world_size] on both axes
    // How far this session can see. Together with shard_count these are the two
    // reasons a client may observe an empty world on a busy server, so they are
    // reported rather than left to be deduced.
    float         aoi_radius;
    std::uint16_t shard_id;
    std::uint16_t shard_count;
    std::uint16_t tick_hz;
    std::uint16_t pad_;
    std::uint32_t pad2_;
};

// client_ts_ns is the client's monotonic clock. The server never interprets it,
// only echoes it, so RTT is computed entirely on one clock -- no cross-machine
// clock-sync error leaks into the latency histogram.
struct Ping {
    std::uint64_t client_ts_ns;
    std::uint64_t cookie;
};

// Both client stamps come back untouched. client_ts_ns carries the time the
// request was *scheduled* to be sent and cookie carries the time it actually
// went out; echoing both lets the client separate its own send-side queueing
// from the server's service time without keeping any per-request state.
struct Pong {
    std::uint64_t client_ts_ns;
    std::uint64_t cookie;
    std::uint64_t server_recv_ns; // server-side arrival, for one-way skew analysis
};

struct MoveCommand {
    std::uint64_t client_ts_ns;
    std::uint32_t input_seq;
    float         dx;
    float         dy;
    std::uint32_t pad_;
};

// Bits in SnakeRecord::flags.
enum SnakeFlags : std::uint16_t {
    // Set on exactly one record per delta: the snake the recipient controls.
    // Without it a client cannot pick itself out of its own area-of-interest
    // list, because ids are assigned by the shard after the handshake has
    // already been answered.
    kSnakeSelf = 1u << 0,
    kSnakeDead = 1u << 1,   // waiting to respawn; draw it as a corpse or not at all
};

// One point of a snake's body. 8 bytes keeps the whole tail 8-aligned, which is
// what lets a client read it in place (see docs/tutorial/03-the-protocol.md).
struct Point2 {
    float x, y;
};

// A snake's header, immediately followed by `segments` Point2 values: the head
// first, then the body trailing behind it.
//
// This is why the state message is a nested variable-length structure rather
// than a flat array: a snake is a polyline, and the number of points differs per
// snake and per tick. Flattening it -- one snapshot per segment with an owner id
// -- would multiply the element count by ~50 and leave the client unable to tell
// where one snake ends and the next begins.
struct SnakeRecord {
    std::uint32_t id;
    std::uint16_t segments;    // Point2 values following this record
    std::uint16_t flags;       // SnakeFlags
    std::uint16_t length;      // target length; grows when this snake eats
    std::uint16_t kills;
    std::uint32_t pad_;
};

struct SnakeDeltaHeader {
    std::uint64_t echo_client_ts_ns; // from the newest MoveCommand folded into this tick
    std::uint32_t tick;
    std::uint32_t input_seq_ack;
    std::uint16_t count;             // SnakeRecord structures following
    std::uint16_t flags;
    std::uint32_t pad_;
};

struct Disconnect {
    std::uint32_t reason;
    std::uint32_t pad_;
};

struct HealthProbe {
    std::uint64_t probe_ts_ns;
    std::uint64_t pad_;
};

struct HealthReply {
    std::uint64_t probe_ts_ns;
    std::uint32_t live_sessions;
    std::uint16_t inflight_pct;      // queue depth as % of capacity, for load-aware routing
    std::uint16_t shard_count;
};

// ------------------------------------------------------- traits & invariants
template <typename T> struct MsgTraits;   // primary template intentionally undefined

// NESTED marks a payload whose tail is a sequence of *variable-sized* records
// rather than a flat array. Such a tail cannot be handed out as a std::span --
// there is no constant stride -- so tail<T>() refuses to compile for it and the
// caller must walk it with a dedicated reader (see SnakeDeltaView).
#define GE_DEFINE_MSG(T, ID, VAR, ELEM) GE_DEFINE_MSG_EX(T, ID, VAR, false, ELEM)

#define GE_DEFINE_MSG_EX(T, ID, VAR, NESTED, ELEM)                             \
    template <> struct MsgTraits<T> {                                          \
        static constexpr MsgType     type      = MsgType::ID;                  \
        static constexpr bool        variable  = VAR;                          \
        static constexpr bool        nested    = NESTED;                       \
        using element = ELEM;                                                  \
    };                                                                         \
    static_assert(std::is_trivially_copyable_v<T>, #T " must be trivially copyable"); \
    static_assert(sizeof(T) % ::ge::proto::kFrameAlign == 0,                   \
                  #T " must be a multiple of the frame alignment");            \
    static_assert(alignof(T) <= ::ge::proto::kFrameAlign,                      \
                  #T " over-aligned for the wire")

struct Void {};
GE_DEFINE_MSG(ClientHello,      ClientHello,   false, Void);
GE_DEFINE_MSG(ServerWelcome,    ServerWelcome, false, Void);
GE_DEFINE_MSG(Ping,             Ping,          false, Void);
GE_DEFINE_MSG(Pong,             Pong,          false, Void);
GE_DEFINE_MSG(MoveCommand,      MoveCommand,   false, Void);
GE_DEFINE_MSG_EX(SnakeDeltaHeader, SnakeDelta, true, true, SnakeRecord);
GE_DEFINE_MSG(Disconnect,       Disconnect,    false, Void);
GE_DEFINE_MSG(HealthProbe,      HealthProbe,   false, Void);
GE_DEFINE_MSG(HealthReply,      HealthReply,   false, Void);
#undef GE_DEFINE_MSG
#undef GE_DEFINE_MSG_EX

static_assert(sizeof(Point2) == 8 && alignof(Point2) == 4);
static_assert(sizeof(SnakeRecord) == 16 && alignof(SnakeRecord) == 4);
static_assert(sizeof(SnakeDeltaHeader) == 24);
static_assert(sizeof(MoveCommand) == 24);
static_assert(sizeof(Ping) == 16 && sizeof(Pong) == 24);
static_assert(sizeof(ServerWelcome) == 32);

// Compile-time proof that every enumerator has exactly one payload type. Adding
// a MsgType without a GE_DEFINE_MSG line is a build error, not a runtime
// "unknown message" branch discovered in production.
template <typename... Ts>
consteval bool msg_table_is_total() {
    bool seen[std::size_t(MsgType::Count)] = {};
    ((seen[std::size_t(MsgTraits<Ts>::type)] = true), ...);
    for (std::size_t i = 1; i < std::size_t(MsgType::Count); ++i)
        if (!seen[i]) return false;
    return sizeof...(Ts) == std::size_t(MsgType::Count) - 1;   // no duplicates
}
static_assert(msg_table_is_total<ClientHello, ServerWelcome, Ping, Pong, MoveCommand,
                                 SnakeDeltaHeader, Disconnect, HealthProbe, HealthReply>(),
              "MsgType enumerators and payload structs are out of sync");

constexpr const char* msg_name(MsgType t) noexcept {
    switch (t) {
        case MsgType::ClientHello:   return "ClientHello";
        case MsgType::ServerWelcome: return "ServerWelcome";
        case MsgType::Ping:          return "Ping";
        case MsgType::Pong:          return "Pong";
        case MsgType::MoveCommand:   return "MoveCommand";
        case MsgType::SnakeDelta:    return "SnakeDelta";
        case MsgType::Disconnect:    return "Disconnect";
        case MsgType::HealthProbe:   return "HealthProbe";
        case MsgType::HealthReply:   return "HealthReply";
        case MsgType::Invalid:
        case MsgType::Count:         break;
    }
    return "Invalid";
}

// Simulation limits that the wire format has to be able to express. A snake is
// capped so that a worst-case delta -- every visible snake at full length -- has
// a known, bounded size, which is what lets every buffer downstream be sized at
// startup.
inline constexpr std::size_t kMaxSnakeSegments = 48;
inline constexpr std::size_t kMaxAoiSnakes     = 24;

inline constexpr std::size_t kMaxFrameBytes =
    sizeof(FrameHeader) + sizeof(SnakeDeltaHeader) +
    kMaxAoiSnakes * (sizeof(SnakeRecord) + kMaxSnakeSegments * sizeof(Point2));
static_assert(kMaxFrameBytes <= kMaxPayload,
              "worst-case snake delta must fit one frame");

} // namespace ge::proto
