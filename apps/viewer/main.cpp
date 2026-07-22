// ge_viewer -- watch the simulation.
//
// This is a real client, not a debug hook into the server: it connects over TCP,
// completes the handshake, sends MoveCommands, and renders whatever the server
// chooses to tell it. What you see on screen is exactly one player's
// area-of-interest view, decoded from the wire format in
// include/ge/proto/codec.hpp, with no privileged access to the simulation's
// memory. If the protocol were broken, this would show it.
//
// By default it also starts a server in-process and a crowd of bot clients, so
// one command gives you something to look at.
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "ge/core/bits.hpp"
#include "ge/core/clock.hpp"
#include "ge/net/socket.hpp"
#include "ge/proto/codec.hpp"
#include "ge/server/world_server.hpp"

namespace {

using ge::core::Clock;
namespace net   = ge::net;
namespace proto = ge::proto;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// ---------------------------------------------------------------- keyboard
//
// A terminal normally hands a program whole lines, after the user presses
// Enter, and echoes what they typed. Neither is wanted here: a game needs the
// keystroke the moment it happens, and echoing it would scribble over the
// frame. Raw mode turns both off.
//
// The saved settings are restored through atexit as well as on the normal path,
// because leaving a terminal in raw mode is a genuinely hostile thing to do to
// someone -- their shell stops echoing and stops responding to Ctrl-C.
class RawTerminal {
public:
    bool enable() {
        if (!::isatty(STDIN_FILENO)) return false;
        if (::tcgetattr(STDIN_FILENO, &saved_) != 0) return false;
        termios raw = saved_;
        raw.c_lflag &= tcflag_t(~(ICANON | ECHO));
        raw.c_cc[VMIN]  = 0;      // read() returns immediately...
        raw.c_cc[VTIME] = 0;      // ...with whatever is available, or nothing
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
        active_ = true;
        return true;
    }
    void restore() {
        if (active_) { ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_); active_ = false; }
    }
private:
    termios saved_{};
    bool    active_ = false;
};

RawTerminal g_raw;
void restore_terminal() { g_raw.restore(); std::printf("\033[?25h"); std::fflush(stdout); }

// Reads whatever keys are pending and folds them into a direction. Returns
// false if the user asked to quit.
bool poll_keys(float& dx, float& dy, bool& changed) {
    char buf[64];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0) return true;

    for (ssize_t i = 0; i < n; ++i) {
        char c = buf[i];
        // Arrow keys arrive as the three bytes ESC '[' 'A'..'D'.
        if (c == '\033' && i + 2 < n && buf[i + 1] == '[') {
            switch (buf[i + 2]) {
                case 'A': dx =  0; dy = -1; changed = true; break;
                case 'B': dx =  0; dy =  1; changed = true; break;
                case 'C': dx =  1; dy =  0; changed = true; break;
                case 'D': dx = -1; dy =  0; changed = true; break;
                default: break;
            }
            i += 2;
            continue;
        }
        switch (c) {
            case 'w': case 'W': case 'k': dx =  0; dy = -1; changed = true; break;
            case 's': case 'S': case 'j': dx =  0; dy =  1; changed = true; break;
            case 'a': case 'A': case 'h': dx = -1; dy =  0; changed = true; break;
            case 'd': case 'D': case 'l': dx =  1; dy =  0; changed = true; break;
            case 'q': case 'Q': case 3:   return false;      // 3 == Ctrl-C
            default: break;
        }
    }
    return true;
}

constexpr std::uint32_t kRecvBuf = 1u << 16;

// ---------------------------------------------------------------- a client
struct Client {
    net::Fd       fd;
    std::vector<std::byte> rbuf_storage;
    std::byte*    rbuf = nullptr;
    std::uint32_t rlen = 0;
    std::uint32_t session = 0;
    std::uint32_t input_seq = 0;
    bool          handshaked = false;
    proto::ServerWelcome welcome{};
    bool          dead = false;
    float         dx = 0, dy = 0;
    std::int64_t  next_turn_ns = 0;
};

// The receive buffer must be 8-byte aligned: the codec casts into it in place.
void alloc_buf(Client& c) {
    c.rbuf_storage.resize(kRecvBuf + 8);
    auto addr = reinterpret_cast<std::uintptr_t>(c.rbuf_storage.data());
    c.rbuf = reinterpret_cast<std::byte*>((addr + 7) & ~std::uintptr_t(7));
}

ge::core::Status connect_client(Client& c, const std::string& host, std::uint16_t port) {
    auto fd = net::connect_tcp(host.c_str(), port, false);
    if (!fd) return std::unexpected(fd.error());
    c.fd = std::move(*fd);
    net::set_nodelay(c.fd.get());
    net::set_nosigpipe(c.fd.get());
    net::set_nonblocking(c.fd.get());
    alloc_buf(c);

    alignas(8) std::byte buf[64];
    const std::size_t n = proto::encode(buf, proto::ClientHello{0, proto::kProtocolVersion, 0}, 0, 0);
    if (net::write_some(c.fd.get(), buf, n).result != net::IoResult::Ok)
        return ge::core::fail("handshake write failed");
    return ge::core::ok();
}

void send_move(Client& c, float dx, float dy) {
    if (c.dead || !c.handshaked) return;
    alignas(8) std::byte buf[64];
    proto::MoveCommand m{};
    m.client_ts_ns = std::uint64_t(Clock::now_ns());
    m.input_seq    = c.input_seq++;
    m.dx = dx; m.dy = dy;
    const std::size_t n = proto::encode(buf, m, c.session, m.input_seq);
    if (net::write_some(c.fd.get(), buf, n).result == net::IoResult::Error) c.dead = true;
}

// One snake as the viewer last saw it.
struct SeenSnake {
    std::uint32_t id = 0;
    std::uint16_t flags = 0, length = 0, kills = 0;
    std::vector<proto::Point2> pts;            // head first
};

// What the viewer currently believes the world looks like.
struct WorldView {
    std::vector<SeenSnake> snakes;
    float         self_x = 0, self_y = 0;
    std::uint32_t self_id = 0;
    std::uint16_t self_len = 0, self_kills = 0;
    bool          self_dead = false;
    bool          have_self = false;
    std::uint32_t tick = 0;
    std::uint64_t deltas = 0;
    std::int64_t  last_rtt_ns = 0;
    // A delta goes out every tick carrying the newest input it has folded in,
    // so the same echo repeats until the next input. Timing every delta against
    // it would measure "time since I last moved", not round-trip latency.
    std::uint64_t last_echo = 0;
    std::uint32_t last_tick_at_mark = 0;
    std::int64_t  tick_rate_mark_ns = 0;
    double        sim_hz = 0;
};

// Reads everything pending and folds it into `view` (for the viewer client) or
// simply drains it (for bots).
void pump(Client& c, WorldView* view) {
    if (c.dead) return;
    for (;;) {
        if (c.rlen >= kRecvBuf) { c.rlen = 0; }          // parser wedged: resync
        const net::IoOutcome r = net::read_some(c.fd.get(), c.rbuf + c.rlen, kRecvBuf - c.rlen);
        if (r.result == net::IoResult::Closed || r.result == net::IoResult::Error) { c.dead = true; return; }
        if (r.result == net::IoResult::Ok) c.rlen += std::uint32_t(r.bytes);

        proto::FrameReader reader(std::span<const std::byte>(c.rbuf, c.rlen));
        proto::FrameView f;
        for (;;) {
            const proto::ParseError e = reader.next(f);
            if (e == proto::ParseError::Incomplete) break;
            if (e != proto::ParseError::Ok) { c.dead = true; return; }

            if (f.type() == proto::MsgType::ServerWelcome) {
                if (const auto* m = f.as<proto::ServerWelcome>()) {
                    c.session = m->session_id;
                    c.welcome = *m;
                    c.handshaked = true;
                }
            } else if (f.type() == proto::MsgType::SnakeDelta && view) {
                if (const auto* h = f.as<proto::SnakeDeltaHeader>()) {
                    // Walk the nested tail. The points alias the receive buffer,
                    // which is about to be reused, so this is the one place the
                    // viewer does copy -- it has to keep the last frame around
                    // to redraw at its own rate.
                    view->snakes.clear();
                    proto::SnakeDeltaView sv(f);
                    const proto::SnakeRecord* sr = nullptr;
                    std::span<const proto::Point2> pts;
                    while (sv.next(sr, pts)) {
                        SeenSnake sn;
                        sn.id = sr->id; sn.flags = sr->flags;
                        sn.length = sr->length; sn.kills = sr->kills;
                        sn.pts.assign(pts.begin(), pts.end());
                        if (sr->flags & proto::kSnakeSelf) {
                            view->have_self  = true;
                            view->self_id    = sr->id;
                            view->self_len   = sr->length;
                            view->self_kills = sr->kills;
                            view->self_dead  = (sr->flags & proto::kSnakeDead) != 0;
                            if (!pts.empty()) { view->self_x = pts[0].x; view->self_y = pts[0].y; }
                        }
                        view->snakes.push_back(std::move(sn));
                    }
                    view->tick = h->tick;
                    ++view->deltas;
                    if (h->echo_client_ts_ns && h->echo_client_ts_ns != view->last_echo) {
                        view->last_echo   = h->echo_client_ts_ns;
                        view->last_rtt_ns = Clock::now_ns() - std::int64_t(h->echo_client_ts_ns);
                    }
                    const std::int64_t now = Clock::now_ns();
                    if (view->tick_rate_mark_ns == 0) {
                        view->tick_rate_mark_ns = now;
                        view->last_tick_at_mark = h->tick;
                    } else if (now - view->tick_rate_mark_ns >= 1'000'000'000) {
                        view->sim_hz = double(h->tick - view->last_tick_at_mark) * 1e9
                                     / double(now - view->tick_rate_mark_ns);
                        view->tick_rate_mark_ns = now;
                        view->last_tick_at_mark = h->tick;
                    }
                }
            }
        }
        const std::size_t used = reader.consumed();
        if (used) { c.rlen -= std::uint32_t(used); if (c.rlen) std::memmove(c.rbuf, c.rbuf + used, c.rlen); }
        if (r.result == net::IoResult::WouldBlock || r.bytes == 0) return;
    }
}

// ---------------------------------------------------------------- rendering
struct Term {
    int cols = 80, rows = 24;
    void measure() {
        winsize w{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 20 && w.ws_row > 10) {
            cols = w.ws_col; rows = w.ws_row;
        }
    }
};

// Renders into a string and writes it in one go. Drawing character by character
// straight to the terminal flickers badly; one write per frame does not.
std::string render(const WorldView& v, const Term& t, float world, int expected,
                   double fps, std::uint64_t frames, const char* controls) {
    (void)fps;
    const int plot_w = std::max(20, t.cols - 2);
    const int plot_h = std::max(8,  t.rows - 6);

    // Every viewer draws the same shared world in absolute coordinates, so the
    // windows *should* look alike -- that is what "shared" means. What must not
    // be alike is which snake is yours. Giving each snake its own colour, keyed
    // on the id the server assigned, makes the windows visibly about different
    // players instead of looking like clones of one another.
    struct Cell { char ch = ' '; std::uint8_t colour = 0; };
    const auto pw = std::size_t(plot_w), ph = std::size_t(plot_h);
    std::vector<Cell> cell(pw * ph);
    // Priority so that a crowded cell shows the most important thing in it:
    // your head, then another head, then your body, then anyone's body.
    auto rank = [](char c) {
        switch (c) { case '@': return 4; case 'O': return 3;
                     case '*': return 2; case 'o': return 1; default: return 0; }
    };
    auto put = [&](int cx, int cy, char ch, std::uint8_t colour) {
        if (cx < 0 || cy < 0 || cx >= plot_w || cy >= plot_h) return;
        Cell& dst = cell[std::size_t(cy) * pw + std::size_t(cx)];
        if (rank(ch) >= rank(dst.ch)) { dst.ch = ch; dst.colour = colour; }
    };

    // World -> character cell.
    //
    // The world is a CLOSED interval [0, world]: the simulation clamps a
    // bouncing entity to exactly the boundary, so x == world does occur. Without
    // the clamp below, world/world*plot_w == plot_w -- one past the last column
    // -- and put() silently drops it. An entity touching a wall therefore
    // vanished for as long as it sat there, which reads as "my character went
    // off screen". Clamping (rather than scaling by plot_w-1) keeps interior
    // points evenly distributed and only folds the closed endpoint inward.
    //
    // It sits there for two ticks, not one: the integrator detects a bounce with
    // `clamped == unclamped`, which is false only once the entity is *already*
    // on the boundary and tries to leave, so the first tick lands on the wall
    // and the second reverses. Harmless for the simulation, but it doubles how
    // long this bug was visible.
    auto to_cell = [&](float x, float y, int& ox, int& oy) {
        ox = std::clamp(int(x / world * float(plot_w)), 0, plot_w - 1);
        oy = std::clamp(int(y / world * float(plot_h)), 0, plot_h - 1);
    };

    // Draw each snake as a connected polyline. Plotting only the body points
    // leaves gaps whenever a snake moves faster than one cell per segment, so
    // consecutive points are joined with a line. Bresenham is used rather than
    // floating-point stepping because it lands on exactly the cells a human
    // would draw and never double-writes one.
    auto line = [&](int x0, int y0, int x1, int y1, char ch, std::uint8_t colour) {
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (int guard = 0; guard < 4096; ++guard) {
            put(x0, y0, ch, colour);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    };

    // Colour 0 is reserved for you; everyone else gets one derived from their
    // id, so the same player is the same colour in every window.
    auto colour_of = [](const SeenSnake& sn) -> std::uint8_t {
        if (sn.flags & proto::kSnakeSelf) return 0;
        return std::uint8_t(1 + (sn.id % 6));
    };

    for (const auto& sn : v.snakes) {
        if (sn.pts.empty() || (sn.flags & proto::kSnakeDead)) continue;
        const bool self = (sn.flags & proto::kSnakeSelf) != 0;
        // Bodies are drawn before heads so a head is never hidden under someone
        // else's tail.
        for (std::size_t k = 1; k < sn.pts.size(); ++k) {
            int ax, ay, bx, by;
            to_cell(sn.pts[k - 1].x, sn.pts[k - 1].y, ax, ay);
            to_cell(sn.pts[k].x,     sn.pts[k].y,     bx, by);
            line(ax, ay, bx, by, self ? '*' : 'o', colour_of(sn));
        }
    }
    for (const auto& sn : v.snakes) {
        if (sn.pts.empty() || (sn.flags & proto::kSnakeDead)) continue;
        int hx, hy;
        to_cell(sn.pts[0].x, sn.pts[0].y, hx, hy);
        put(hx, hy, (sn.flags & proto::kSnakeSelf) ? '@' : 'O', colour_of(sn));
    }

    std::string out;
    out.reserve((pw + 4) * (ph + 8));
    out += "\033[H";                                   // home, no clear: less flicker

    char hdr[256];
    std::snprintf(hdr, sizeof hdr,
        "\033[1mge_snake\033[0m  \033[1;32mYOU = snake #%u\033[0m at (%.0f,%.0f)  "
        "len %-3u kills %-3u%s  snakes %-3zu%-5s  tick %-6u  %4.1f Hz  "
        "in->view %5.1f ms\033[K\n",
        v.self_id, double(v.self_x), double(v.self_y),
        v.self_len, v.self_kills, v.self_dead ? "  \033[1;31mDEAD\033[0m" : "      ",
        v.snakes.size(),
        // Only meaningful when we started the server and know the population.
        expected > 0 ? ("/" + std::to_string(expected)).c_str() : "",
        v.tick, v.sim_hz, double(v.last_rtt_ns) / 1e6);
    out += hdr;

    out += "\033[2m+";
    out.append(pw, '-');
    out += "+\033[0m\033[K\n";

    for (std::size_t y = 0; y < ph; ++y) {
        out += "\033[2m|\033[0m";
        for (std::size_t x = 0; x < pw; ++x) {
            static const char* kHead[7] = {"1;32", "1;31", "1;33", "1;34",
                                           "1;35", "1;36", "1;91"};
            static const char* kBody[7] = {"32", "31", "33", "34", "35", "36", "91"};
            const Cell& c2 = cell[y * pw + x];
            if (c2.ch == ' ') { out += ' '; continue; }
            const bool head = (c2.ch == '@' || c2.ch == 'O');
            out += "\033[";
            out += head ? kHead[c2.colour] : kBody[c2.colour];
            out += 'm';
            out += c2.ch;
            out += "\033[0m";
        }
        out += "\033[2m|\033[0m\033[K\n";
    }

    out += "\033[2m+";
    out.append(pw, '-');
    out += "+\033[0m\033[K\n";

    char foot[256];
    std::snprintf(foot, sizeof foot,
        "\033[2m\033[1;32m@\033[0m\033[2m = you (green). Other colours are other players; "
        "every window shows the SAME world, so only the green snake is yours.   "
        "%s   world %.0fx%.0f   frame %llu\033[0m\033[K\n",
        controls, double(world), double(world),
        static_cast<unsigned long long>(frames));
    out += foot;
    return out;
}

[[noreturn]] void usage(const char* a0) {
    std::printf(
"usage: %s [options]\n"
"    --bots N        other players to spawn      (default 24)\n"
"    --fps N         redraw rate                 (default 20)\n"
"    --world W       world size for the in-process server (default 256);\n"
"                    ignored with --host: the server reports its own\n"
"    --tick-hz N     server simulation rate      (default 30)\n"
"    --host H        attach to an external server instead of starting one\n"
"    --port P        port for --host\n"
"    --auto          let your own snake wander instead of steering it\n"
"\n"
"  Controls: WASD or arrow keys to steer, q to quit.\n", a0);
    std::exit(2);
}

} // namespace

int main(int argc, char** argv) {
    int   bots = 24, fps = 20, tick_hz = 30;
    bool  manual = true;                 // steer with the keyboard when on a tty
    float world = 256.0f;
    std::string host;
    std::uint16_t port = 0;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { if (i + 1 >= argc) usage(argv[0]); return argv[++i]; };
        if      (!std::strcmp(argv[i], "--bots"))    bots = std::atoi(next());
        else if (!std::strcmp(argv[i], "--fps"))     fps = std::max(1, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--world"))   world = float(std::atof(next()));
        else if (!std::strcmp(argv[i], "--tick-hz")) tick_hz = std::atoi(next());
        else if (!std::strcmp(argv[i], "--auto"))    manual = false;
        else if (!std::strcmp(argv[i], "--host"))    host = next();
        else if (!std::strcmp(argv[i], "--port"))    port = std::uint16_t(std::atoi(next()));
        else usage(argv[0]);
    }

    // Validate before anything is constructed. Bad values here used to fail in
    // ways that were hard to attribute: --bots -1 produced a zero-length client
    // vector and rendered an empty box forever, --world 0 divided by zero in the
    // coordinate mapping, and --tick-hz 0 hit integer division by zero inside
    // TickScheduler (silent free-running loop on arm64, SIGFPE on x86-64).
    auto bad = [&](const char* what) {
        std::fprintf(stderr, "ge_viewer: %s\n", what);
        std::exit(2);
    };
    if (bots < 0)      bad("--bots must be >= 0");
    if (bots > 4096)   bad("--bots must be <= 4096");
    if (!(world > 0))  bad("--world must be > 0");
    if (tick_hz <= 0)  bad("--tick-hz must be > 0");
    if (!host.empty() && port == 0) bad("--host needs --port");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    ge::server::WorldServer server;
    const bool inproc = host.empty();
    if (inproc) {
        ge::server::ServerConfig cfg;
        cfg.port         = 0;                 // ephemeral
        cfg.io_threads   = 1;
        cfg.shards       = 1;                 // one shard: everyone can see everyone
        cfg.tick_hz      = tick_hz;
        cfg.world_size   = world;
        cfg.cell_size    = world / 8.0f;
        // An area of interest big enough to cover the whole world, so the view
        // is the entire simulation rather than one player's neighbourhood.
        cfg.aoi_radius   = world * 2.0f;
        cfg.conns_per_io = std::uint32_t(bots + 8);
        cfg.io_spin_ns   = 500'000;
        if (auto started = server.start(cfg); !started) {
            std::fprintf(stderr, "server start failed: %s\n", started.error().c_str());
            return 1;
        }
        host = "127.0.0.1";
        port = server.port();
    }

    // One viewer plus the bots. The viewer is just another client.
    std::vector<Client> clients(std::size_t(bots) + 1);
    for (std::size_t i = 0; i < clients.size(); ++i) {
        if (auto r = connect_client(clients[i], host, port); !r) {
            std::fprintf(stderr, "connect failed: %s\n", r.error().c_str());
            return 1;
        }
    }

    WorldView view;
    const auto t_start = Clock::now_ns();
    while (Clock::now_ns() - t_start < 2'000'000'000 && !g_stop) {
        for (std::size_t i = 0; i < clients.size(); ++i)
            pump(clients[i], i == 0 ? &view : nullptr);
        if (std::all_of(clients.begin(), clients.end(),
                        [](const Client& c) { return c.handshaked; })) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // The world's dimensions are the server's business, not a flag the user has
    // to keep in sync. Take them from the handshake; --world only seeds the
    // in-process server. Getting this wrong silently was the whole problem: a
    // viewer defaulting to 256 against a 1024-wide server crams everything into
    // a quarter of the screen and folds the rest onto the last column.
    const proto::ServerWelcome& w = clients[0].welcome;
    if (clients[0].handshaked && w.world_size > 0) world = w.world_size;

    // Explain up front why the view may be empty, rather than leaving the user
    // to conclude the server is broken. These are the only two reasons.
    if (!inproc) {
        std::printf("attached to %s:%u -- world %.0fx%.0f, aoi %.0f, shard %u of %u, %u Hz\n",
                    host.c_str(), port, double(w.world_size), double(w.world_size),
                    double(w.aoi_radius), w.shard_id, w.shard_count, w.tick_hz);
        bool isolated = false;
        if (w.shard_count > 1) {
            std::printf("  note: this server runs %u shards. You are on shard %u and can only\n"
                        "        see players the session hash placed there. Restart it with\n"
                        "        --shards 1 for a single shared world.\n",
                        w.shard_count, w.shard_id);
            isolated = true;
        }
        if (w.aoi_radius < w.world_size) {
            std::printf("  note: area of interest %.0f is smaller than the world %.0f, so two\n"
                        "        players are rarely in range. Restart the server with --aoi %.0f.\n",
                        double(w.aoi_radius), double(w.world_size), double(w.world_size * 2));
            isolated = true;
        }
        if (isolated) {
            std::printf("\nStarting in 3s...\n");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    ge::core::Xoshiro256ss rng(0xC0FFEE);
    Term term;
    term.measure();

    // Only take over the terminal if we are actually attached to one; piping
    // the output (as the capture scripts do) must still work.
    const bool steering = manual && g_raw.enable();
    std::atexit(restore_terminal);

    std::printf("\033[2J\033[?25l");           // clear once, hide cursor

    const std::int64_t frame_ns = 1'000'000'000 / fps;
    std::int64_t next_frame = Clock::now_ns();
    std::int64_t last_fps_t = Clock::now_ns();
    std::uint64_t frames = 0, frames_at_mark = 0;
    double fps_now = 0;

    while (!g_stop) {
        const std::int64_t now = Clock::now_ns();

        // Everyone picks a new heading now and then, so the world keeps moving.
        //
        // Purely random headings made walls the leading cause of death, which is
        // dull to watch and buries the mechanic under scenery. Near an edge the
        // bots steer back towards the middle; otherwise they wander.
        // Your snake, steered by hand. A key sets the heading you want; the
        // server turns you towards it at a limited rate, so holding a direction
        // is a request rather than a teleport.
        if (steering) {
            float kdx = 0, kdy = 0;
            bool  pressed = false;
            if (!poll_keys(kdx, kdy, pressed)) break;
            if (pressed && !clients[0].dead) {
                send_move(clients[0], kdx, kdy);
                // Screen y grows downward, world y grows downward too, so the
                // mapping is direct and "up" really is up.
                clients[0].next_turn_ns = now + 1'000'000'000;   // suppress wandering
            }
        }

        for (std::size_t ci = steering ? 1 : 0; ci < clients.size(); ++ci) {
            Client& c = clients[ci];
            if (c.dead || now < c.next_turn_ns) continue;

            float ang;
            const float margin = world * 0.22f;
            const bool near_edge = (view.self_x < margin || view.self_x > world - margin ||
                                    view.self_y < margin || view.self_y > world - margin);
            if (!near_edge) {
                ang = float(rng.next_double() * 6.2831853);
            } else {
                // Aim at the centre, with a little slop so they do not converge
                // on one point and pile up.
                ang = std::atan2(world * 0.5f - view.self_y, world * 0.5f - view.self_x)
                    + float(rng.next_double() - 0.5) * 1.2f;
            }
            send_move(c, std::cos(ang), std::sin(ang));
            c.next_turn_ns = now + std::int64_t(250'000'000 + rng.next_below(900'000'000));
        }

        for (std::size_t i = 0; i < clients.size(); ++i)
            pump(clients[i], i == 0 ? &view : nullptr);

        if (now >= next_frame) {
            term.measure();                    // handle a resized window
            const std::string frame = render(view, term, world, inproc ? bots + 1 : 0,
                                             fps_now, frames,
                                             steering ? "WASD/arrows to steer, q quits"
                                                      : "auto-steering");
            // glibc marks write() warn_unused_result. A short or failed write
            // to a terminal is not worth recovering from -- the next frame
            // overwrites it -- but the result must be consumed deliberately
            // rather than by ignoring the warning.
            const ssize_t wrote = ::write(STDOUT_FILENO, frame.data(), frame.size());
            (void)wrote;
            ++frames;
            next_frame = now + frame_ns;
            if (now - last_fps_t >= 1'000'000'000) {
                fps_now = double(frames - frames_at_mark) * 1e9 / double(now - last_fps_t);
                frames_at_mark = frames;
                last_fps_t = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    restore_terminal();
    std::printf("\n");
    std::fflush(stdout);

    if (inproc) {
        const auto rep = server.report();
        std::printf("%llu ticks, %llu kills, %llu deaths (the difference is walls), "
                    "%llu deltas built, %llu dropped\n",
                    static_cast<unsigned long long>(rep.shard.ticks),
                    static_cast<unsigned long long>(rep.shard.kills),
                    static_cast<unsigned long long>(rep.shard.deaths),
                    static_cast<unsigned long long>(rep.shard.deltas_built),
                    static_cast<unsigned long long>(rep.shard.deltas_dropped));
        server.stop();
        server.join();
    }
    return 0;
}
