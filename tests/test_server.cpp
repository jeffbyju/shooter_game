#include "test.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <unistd.h>

#include "ge/net/socket.hpp"
#include "ge/proto/codec.hpp"
#include "ge/server/world_server.hpp"

using namespace ge;

namespace {

server::ServerConfig small_server() {
    server::ServerConfig c;
    c.host            = "127.0.0.1";
    c.port            = 0;            // ephemeral: never collides with a live service
    c.io_threads      = 1;
    c.shards          = 1;
    c.tick_hz         = 120;
    c.conns_per_io    = 8;            // small, so slot recycling is actually exercised
    c.read_buf_bytes  = 4096;
    c.write_buf_bytes = 4096;
    c.poll_timeout_ms = 5;
    return c;
}

// Connects, handshakes, and returns the fd. -1 if the server refused.
int connect_and_hello(std::uint16_t port) {
    auto fd = net::connect_tcp("127.0.0.1", port, false);
    if (!fd) return -1;
    net::set_nodelay(fd->get());
    net::set_nosigpipe(fd->get());

    alignas(8) std::byte buf[64];
    const std::size_t n = proto::encode(buf, proto::ClientHello{1, proto::kProtocolVersion, 0}, 0, 0);
    if (net::write_some(fd->get(), buf, n).result != net::IoResult::Ok) return -1;
    return fd->release();
}

// Reads until a ServerWelcome arrives or the deadline passes.
bool await_welcome(int fd, int timeout_ms) {
    alignas(8) std::byte buf[4096];
    std::uint32_t len = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const net::IoOutcome r = net::read_some(fd, buf + len, sizeof buf - len);
        if (r.result == net::IoResult::Closed || r.result == net::IoResult::Error) return false;
        if (r.result == net::IoResult::Ok) {
            len += std::uint32_t(r.bytes);
            proto::FrameReader reader(std::span<const std::byte>(buf, len));
            proto::FrameView f;
            while (reader.next(f) == proto::ParseError::Ok)
                if (f.type() == proto::MsgType::ServerWelcome) return true;
            len = 0;   // only the handshake matters here; discard the rest
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

} // namespace

// Regression test for a heap corruption found by the load sweep.
//
// A Disconnect arriving in the same TCP segment as later frames used to close
// the connection mid-parse and then fall through to consume(reader.consumed()).
// The close had already reset the buffer, so consume() subtracted a stale byte
// count from zero, underflowed rlen_ to ~4e9, and memmove walked off the end of
// the receive buffer. The bug needed two frames in one segment to reproduce,
// which is why it survived every hand-driven test and only appeared under load.
GE_TEST(server_survives_disconnect_followed_by_more_frames_in_one_segment) {
    server::WorldServer srv;
    CHECK(srv.start(small_server()).has_value());

    const int fd = connect_and_hello(srv.port());
    CHECK(fd >= 0);
    CHECK(await_welcome(fd, 2000));

    // One write: ping, disconnect, then two more pings the server must not parse.
    alignas(8) std::byte batch[512];
    std::size_t off = 0;
    off += proto::encode(std::span<std::byte>(batch + off, sizeof batch - off),
                         proto::Ping{1, 1}, 1, 0);
    off += proto::encode(std::span<std::byte>(batch + off, sizeof batch - off),
                         proto::Disconnect{0, 0}, 1, 1);
    off += proto::encode(std::span<std::byte>(batch + off, sizeof batch - off),
                         proto::Ping{2, 2}, 1, 2);
    off += proto::encode(std::span<std::byte>(batch + off, sizeof batch - off),
                         proto::Ping{3, 3}, 1, 3);
    CHECK(net::write_some(fd, batch, off).result == net::IoResult::Ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ::close(fd);

    const auto rep = srv.report();
    CHECK(rep.io.closes >= 1);
    CHECK_EQ(rep.io.parse_errors, 0u);   // the batch was well formed; the close was deliberate

    srv.stop();
    srv.join();
}

// Regression test for a connection slot and buffer leak.
//
// close_connection never returned the slot to its free list and every accept
// allocated fresh buffers from the thread's arena, so a server could serve only
// conns_per_io connections in its lifetime. The load sweep hit this on its third
// point. Cycling more connections than the slab holds is the direct check.
GE_TEST(server_recycles_connection_slots_across_many_cycles) {
    server::ServerConfig cfg = small_server();   // conns_per_io == 8
    server::WorldServer srv;
    CHECK(srv.start(cfg).has_value());

    constexpr int kCycles = 40;                  // 5x the slab size
    int handshakes = 0;
    for (int i = 0; i < kCycles; ++i) {
        const int fd = connect_and_hello(srv.port());
        if (fd < 0) continue;
        if (await_welcome(fd, 1000)) ++handshakes;
        ::close(fd);
        // Give the server a moment to notice the close and reclaim the slot.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // Without slot recycling this stalls at 8.
    CHECK(handshakes > kCycles / 2);

    const auto rep = srv.report();
    CHECK(rep.io.accepts >= std::uint64_t(handshakes));
    CHECK_EQ(rep.io.parse_errors, 0u);

    srv.stop();
    srv.join();
}

GE_TEST(server_rejects_malformed_frames_without_dying) {
    server::WorldServer srv;
    CHECK(srv.start(small_server()).has_value());

    auto conn = net::connect_tcp("127.0.0.1", srv.port(), false);
    CHECK(conn.has_value());
    net::Fd fd = std::move(*conn);

    // Valid magic, nonsense everything else: the parser must reject and close
    // rather than trusting a length or a type it did not validate.
    alignas(8) std::byte junk[128];
    std::memset(junk, 0xAB, sizeof junk);
    const std::uint32_t magic = proto::kMagic;
    std::memcpy(junk, &magic, sizeof magic);
    net::write_some(fd.get(), junk, sizeof junk);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto rep = srv.report();
    CHECK(rep.io.parse_errors >= 1);
    CHECK(rep.io.closes >= 1);

    // And the server is still serving: a well-formed client connects after it.
    const int good = connect_and_hello(srv.port());
    CHECK(good >= 0);
    CHECK(await_welcome(good, 2000));
    ::close(good);

    srv.stop();
    srv.join();
}

GE_TEST(server_reports_zero_steady_state_allocations_while_serving) {
    if (!alloc::probe_enabled()) return;

    server::WorldServer srv;
    CHECK(srv.start(small_server()).has_value());

    const int fd = connect_and_hello(srv.port());
    CHECK(fd >= 0);
    CHECK(await_welcome(fd, 2000));

    // Measure only after the connection is established, so the accept path's
    // one-time costs are outside the window.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    alloc::AllocScope scope;

    alignas(8) std::byte buf[64];
    alignas(8) std::byte sink[8192];
    for (int i = 0; i < 200; ++i) {
        const std::size_t n = proto::encode(buf, proto::Ping{std::uint64_t(i), 0}, 1, std::uint32_t(i));
        if (net::write_some(fd, buf, n).result != net::IoResult::Ok) break;
        net::read_some(fd, sink, sizeof sink);          // drain so nothing backs up
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    const auto d = scope.delta();
    ::close(fd);

    // The whole process is counted, this test thread included, so a handful of
    // allocations from test scaffolding would show up here. The server's own
    // serving path contributes none.
    CHECK_EQ(d.allocs, 0u);

    srv.stop();
    srv.join();
}
