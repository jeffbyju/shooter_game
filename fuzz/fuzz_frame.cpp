// Coverage-guided fuzzing of the wire format.
//
// The codec casts structs in place rather than parsing field by field, so the
// bounds and alignment checks in FrameReader are the only thing between a
// hostile datagram and a wild read. Hand-written malformed cases cover what I
// thought of; this covers what I did not.
//
//   clang++ -O1 -g -std=c++23 -fsanitize=fuzzer,address,undefined \
//           -Iinclude fuzz/fuzz_frame.cpp -o fuzz_frame
//   ./fuzz_frame -max_total_time=60
#include "ge/proto/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace ge::proto;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 64 * 1024) return 0;
    // FrameReader requires 8-byte alignment, which a real socket buffer always
    // has. Copy into an aligned buffer rather than relaxing the assert.
    alignas(8) static std::byte buf[64 * 1024];
    std::memcpy(buf, data, size);

    FrameReader reader({buf, size});
    FrameView f;
    for (int guard = 0; guard < 4096; ++guard) {
        const ParseError e = reader.next(f);
        if (e != ParseError::Ok) break;

        // Walk whatever the frame claims to be. Every one of these is a cast
        // over attacker-controlled length and count fields.
        switch (f.type()) {
            case MsgType::ClientHello:   (void)f.as<ClientHello>();   break;
            case MsgType::ServerWelcome: (void)f.as<ServerWelcome>(); break;
            case MsgType::Ping:          (void)f.as<Ping>();          break;
            case MsgType::Pong:          (void)f.as<Pong>();          break;
            case MsgType::ShooterInput:  (void)f.as<ShooterInput>();  break;
            case MsgType::ShooterDelta: {
                const ShooterDeltaView d(f);
                if (d.valid()) {
                    std::uint64_t acc = 0;
                    for (const PlayerRecord& r : d.players()) acc += r.id;
                    for (const PickupRecord& r : d.pickups()) acc += r.id;
                    asm volatile("" :: "r"(acc));
                }
                break;
            }
            case MsgType::SnakeDelta: {
                SnakeDeltaView v(f);
                const SnakeRecord* sr = nullptr;
                std::span<const Point2> pts;
                while (v.next(sr, pts)) {
                    float acc = 0;
                    for (const Point2& p : pts) acc += p.x;
                    asm volatile("" :: "r"(acc));
                }
                break;
            }
            default: break;
        }
    }
    return 0;
}
