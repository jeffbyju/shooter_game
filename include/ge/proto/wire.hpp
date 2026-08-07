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
    ShooterInput,       // C -> S  arena mode: movement, look angles and buttons
    ShooterDelta,       // S -> C  arena mode: PlayerRecord[] then PickupRecord[]
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

// ------------------------------------------------------ arena (3D shooter)
//
// A second game mode on the same transport. Snake is 2D and its state message
// is a nested variable-length structure because a snake is a polyline; an arena
// player is a single point with two look angles, so the delta is a *flat* array
// and the client reads it with a fixed stride instead of a bespoke walker.
// Nothing else about the framing changes.

// Bits in ShooterInput::buttons.
enum ShooterButtons : std::uint32_t {
    kBtnFire  = 1u << 0,
    kBtnJump  = 1u << 1,
    // Held while the jetpack is thrusting. The *gesture* that arms it -- a
    // double tap of space, then hold -- is recognised on the client, because
    // only the client has the key-event timestamps and only the client can
    // react to the second tap without a round trip. What crosses the wire is a
    // plain held bit, which the server gates on fuel. A client that simply
    // asserts this bit forever therefore gains nothing: it burns the same fuel
    // at the same rate and stops flying at the same moment.
    kBtnJet   = 1u << 2,
};

// Look angles are sent as absolute radians rather than deltas. A dropped input
// then costs one stale frame instead of permanently desynchronising the view,
// which matters because this is UDP-shaped gameplay running over TCP: a stall
// delivers a burst of inputs at once and relative aiming would integrate all of
// them.
struct ShooterInput {
    std::uint64_t client_ts_ns;
    std::uint32_t input_seq;
    float         move_x;      // -1..1, strafe, relative to yaw
    float         move_z;      // -1..1, forward, relative to yaw
    float         yaw;         // radians, absolute
    float         pitch;       // radians, absolute, clamped server-side
    std::uint32_t buttons;     // ShooterButtons
};

// Bits in PlayerRecord::flags.
enum PlayerFlags : std::uint16_t {
    kPlayerSelf  = 1u << 0,   // exactly one per delta: the recipient's own player
    kPlayerDead  = 1u << 1,
    kPlayerFired = 1u << 2,   // fired this tick; the client draws a tracer along its view ray
    kPlayerHit   = 1u << 3,   // took damage this tick
    kPlayerJet   = 1u << 4,   // jetpack thrusting this tick; the client draws a plume
    kPlayerPick  = 1u << 5,   // collected a powerup this tick
};

// 32 bytes, fixed size -- which is what makes the delta a flat array.
struct PlayerRecord {
    std::uint32_t id;
    float         x, y, z;
    float         yaw, pitch;
    std::uint16_t health;
    std::uint16_t score;
    std::uint16_t flags;       // PlayerFlags
    // Jetpack fuel and ammunition. Both fit in a byte -- fuel is a percentage
    // and ammo is capped at world::kMaxAmmo -- so they occupy the two bytes
    // that were already padding. A delta carrying jetpack and ammo state is
    // not one byte larger than the delta that carried neither, which is the
    // point of having fixed the record at 32 bytes in the first place.
    std::uint8_t  fuel;        // 0..100, percent of world::kMaxFuel
    std::uint8_t  ammo;        // 0..world::kMaxAmmo
};

// ---------------------------------------------------------------- powerups
// What a pickup is on the wire. The server owns every one of them: a client
// never claims a pickup, it is told that one went away and that its own fuel
// or ammo went up. Collection is therefore not something a client can fake.
enum class PickupKind : std::uint8_t {
    Fuel = 0,       // refills the jetpack
    Ammo = 1,       // refills the magazine
};

enum PickupFlags : std::uint8_t {
    kPickupActive = 1u << 0,   // on the floor and collectable right now
};

// 16 bytes: a second flat array, same reasoning as PlayerRecord. Inactive
// pickups are still sent, with kPickupActive clear, so that a client which
// missed the delta that removed one is corrected by the very next delta
// instead of drawing a ghost until the pickup respawns. Full state beats an
// event over a lossy transport -- the same argument that made the delta a
// snapshot rather than a diff.
struct PickupRecord {
    float         x, y, z;
    std::uint16_t id;
    std::uint8_t  kind;        // PickupKind
    std::uint8_t  flags;       // PickupFlags
};

// The tail is TWO flat arrays back to back: `count` PlayerRecords, then
// `pickup_count` PickupRecords. Each array has a constant stride but the tail
// as a whole does not, so the message is marked nested below and tail<T>()
// refuses to form a single span over it. Read it with proto::ShooterDeltaView,
// which does the split once and bounds both arrays by the physical payload
// length rather than by the counts the sender claimed.
struct ShooterDeltaHeader {
    std::uint64_t echo_client_ts_ns;
    std::uint32_t tick;
    std::uint32_t input_seq_ack;
    std::uint16_t count;        // PlayerRecord structures following
    std::uint16_t flags;
    std::uint16_t pickup_count; // PickupRecord structures following those
    std::uint16_t pad_;
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
GE_DEFINE_MSG(ShooterInput,     ShooterInput,  false, Void);
GE_DEFINE_MSG_EX(ShooterDeltaHeader, ShooterDelta, true, true, PlayerRecord);
#undef GE_DEFINE_MSG
#undef GE_DEFINE_MSG_EX

static_assert(sizeof(Point2) == 8 && alignof(Point2) == 4);
static_assert(sizeof(SnakeRecord) == 16 && alignof(SnakeRecord) == 4);
static_assert(sizeof(SnakeDeltaHeader) == 24);
static_assert(sizeof(MoveCommand) == 24);
static_assert(sizeof(Ping) == 16 && sizeof(Pong) == 24);
static_assert(sizeof(ServerWelcome) == 32);
static_assert(sizeof(ShooterInput) == 32 && alignof(ShooterInput) <= 8);
static_assert(sizeof(PlayerRecord) == 32 && alignof(PlayerRecord) == 4);
static_assert(sizeof(PickupRecord) == 16 && alignof(PickupRecord) == 4);
static_assert(sizeof(ShooterDeltaHeader) == 24);
// Both tail elements must keep the payload 8-aligned on their own, or the
// boundary between the two arrays -- and every frame after this one in a TCP
// stream -- stops being aligned and the zero-copy cast stops being legal.
static_assert(sizeof(PlayerRecord) % kFrameAlign == 0);
static_assert(sizeof(PickupRecord) % kFrameAlign == 0);

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
                                 SnakeDeltaHeader, Disconnect, HealthProbe, HealthReply,
                                 ShooterInput, ShooterDeltaHeader>(),
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
        case MsgType::ShooterInput:  return "ShooterInput";
        case MsgType::ShooterDelta:  return "ShooterDelta";
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
inline constexpr std::size_t kMaxAoiPlayers    = 32;
// Powerups in play at once. Every delta carries all of them -- 12 x 16 B =
// 192 bytes, about 15% on top of a 32-player delta -- which buys the loss
// tolerance described on PickupRecord. Sending only changes would be smaller
// and would need an acknowledgement scheme to stay correct under loss, which
// is the complexity this protocol was shaped to avoid.
inline constexpr std::size_t kMaxPickups      = 12;

inline constexpr std::size_t kMaxSnakeFrameBytes =
    sizeof(FrameHeader) + sizeof(SnakeDeltaHeader) +
    kMaxAoiSnakes * (sizeof(SnakeRecord) + kMaxSnakeSegments * sizeof(Point2));
inline constexpr std::size_t kMaxArenaFrameBytes =
    sizeof(FrameHeader) + sizeof(ShooterDeltaHeader) +
    kMaxAoiPlayers * sizeof(PlayerRecord) +
    kMaxPickups * sizeof(PickupRecord);

// Downstream buffers are sized once at startup from this, so it has to bound
// the worst case of *every* mode the server can be run in, not just the default.
inline constexpr std::size_t kMaxFrameBytes =
    kMaxSnakeFrameBytes > kMaxArenaFrameBytes ? kMaxSnakeFrameBytes
                                              : kMaxArenaFrameBytes;
static_assert(kMaxFrameBytes <= kMaxPayload,
              "worst-case delta must fit one frame");

} // namespace ge::proto
