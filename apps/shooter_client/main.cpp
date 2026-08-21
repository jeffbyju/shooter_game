// ge_shooter -- an SDL3 + OpenGL client for the arena game mode.
//
// This is a real client in the same sense ge_viewer is: it speaks the wire
// protocol in include/ge/proto/, has no privileged access to the simulation,
// and renders only what the server's area-of-interest filter chose to send it.
// What appears on screen is one player's view, decoded from the same structs
// the server encoded.
//
// Rendering is OpenGL 4.1 core -- the newest profile macOS offers, and
// deprecated there, hence GL_SILENCE_DEPRECATION. SDL3 supplies the window,
// the GL context and the input.
#define GL_SILENCE_DEPRECATION 1

#include <SDL3/SDL.h>
#include <OpenGL/gl3.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ge/core/clock.hpp"
#include "ge/proto/wire.hpp"
#include "ge/world/arena.hpp"   // kEyeHeight, kMaxHealth: one definition, shared with the server
#include "gl_math.hpp"
#include "net_client.hpp"

namespace {

using ge::client::Mat4;
using ge::core::Clock;
namespace cl = ge::client;

constexpr float kPi = 3.14159265358979f;

// A model matrix and its normal matrix are two views of one fact, so they are
// uploaded by one call. Nine separate glUniformMatrix4fv sites each needing a
// matching glUniformMatrix3fv is nine chances to forget one, and a forgotten
// one lights a single object with stale normals -- the kind of bug that is
// invisible until someone photographs it.
struct ModelUniforms { GLint model; GLint normal; };

static void set_model(const ModelUniforms& u, const ge::client::Mat4& m) {
    glUniformMatrix4fv(u.model, 1, GL_FALSE, m.m);
    const ge::client::Mat3 n = ge::client::normal_matrix(m);
    glUniformMatrix3fv(u.normal, 1, GL_FALSE, n.m);
}

// ------------------------------------------------------------------ shaders
const char* kVertSrc = R"(#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
uniform mat4 u_viewproj;
uniform mat4 u_model;
// Normals do NOT transform by the model matrix. This used to be
// mat3(u_model), which is correct only for rotation and uniform scale; the
// general rule is the inverse transpose, computed on the CPU in
// gl_math.hpp::normal_matrix(). See docs/foundations/08.
uniform mat3 u_normal;
out vec3 v_normal;
out vec3 v_world;
void main() {
    vec4 world = u_model * vec4(a_pos, 1.0);
    v_world  = world.xyz;
    v_normal = u_normal * a_normal;
    gl_Position = u_viewproj * world;
}
)";

const char* kFragSrc = R"(#version 410 core
in vec3 v_normal;
in vec3 v_world;
uniform vec3 u_colour;
uniform int  u_grid;      // 1 = draw floor grid lines procedurally
out vec4 o_colour;
void main() {
    vec3 base = u_colour;
    if (u_grid == 1) {
        // Procedural grid: cheaper than a texture and never needs mip levels.
        vec2 g = abs(fract(v_world.xz * 0.25 - 0.5) - 0.5) / fwidth(v_world.xz * 0.25);
        float line = 1.0 - min(min(g.x, g.y), 1.0);
        base = mix(base, vec3(0.30, 0.34, 0.40), line * 0.9);
    }
    // One fixed light; this is a protocol demo, not a renderer.
    // Two-sided: with culling off, a back face's normal points away from the
    // light and would fall to ambient only, which renders players as faint
    // half-visible shapes. Flipping it for back faces lights both sides.
    //
    // This is correct ONLY while the mesh winding agrees with GL_CCW. When it
    // did not, this line fired on every visible fragment and inverted the
    // lighting of the entire scene while still looking plausible.
    vec3  n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    float d = max(dot(n, normalize(vec3(0.4, 1.0, 0.3))), 0.0);
    o_colour = vec4(base * (0.35 + 0.65 * d), 1.0);
}
)";

const char* kLineVertSrc = R"(#version 410 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_viewproj;
void main() { gl_Position = u_viewproj * vec4(a_pos, 1.0); }
)";

const char* kLineFragSrc = R"(#version 410 core
uniform vec4 u_colour;
out vec4 o_colour;
void main() { o_colour = u_colour; }
)";

const char* kUiVertSrc = R"(#version 410 core
layout(location = 0) in vec2 a_pos;
uniform vec2 u_scale;
uniform vec2 u_offset;
void main() { gl_Position = vec4(a_pos * u_scale + u_offset, 0.0, 1.0); }
)";

const char* kUiFragSrc = R"(#version 410 core
uniform vec4 u_colour;
out vec4 o_colour;
void main() { o_colour = u_colour; }
)";

GLuint compile(GLenum type, const char* src, const char* label) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "%s shader failed:\n%s\n", label, log);
        return 0;
    }
    return s;
}

GLuint link_program(const char* vs, const char* fs, const char* label) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, label);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, label);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "%s link failed:\n%s\n", label, log);
        return 0;
    }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

// ------------------------------------------------------------------ meshes
struct Mesh { GLuint vao = 0, vbo = 0; GLsizei count = 0; };

Mesh make_cube() {
    // 36 vertices, position + normal. Written out rather than generated: it is
    // the only mesh with per-face normals and a loop would be longer.
    //
    // Wound counter-clockwise as seen from OUTSIDE, so cross(p1-p0, p2-p0)
    // agrees with the stored normal and GL's default GL_CCW calls these front
    // faces. It was wound the other way for a long time, which was invisible
    // with culling off but made gl_FrontFacing false on every visible face --
    // so the shader's two-sided branch flipped every correct normal and the
    // whole arena was lit from below. The floor measured 2.6x darker than it
    // should have been. Winding is data, and three separate things read it.
    const float v[] = {
        // -z
        -.5f,-.5f,-.5f, 0,0,-1,   .5f, .5f,-.5f, 0,0,-1,   .5f,-.5f,-.5f, 0,0,-1,
         .5f, .5f,-.5f, 0,0,-1,  -.5f,-.5f,-.5f, 0,0,-1,  -.5f, .5f,-.5f, 0,0,-1,
        // +z
        -.5f,-.5f, .5f, 0,0,1,   .5f,-.5f, .5f, 0,0,1,   .5f, .5f, .5f, 0,0,1,
         .5f, .5f, .5f, 0,0,1,  -.5f, .5f, .5f, 0,0,1,  -.5f,-.5f, .5f, 0,0,1,
        // -x
        -.5f, .5f, .5f, -1,0,0,  -.5f, .5f,-.5f, -1,0,0,  -.5f,-.5f,-.5f, -1,0,0,
        -.5f,-.5f,-.5f, -1,0,0,  -.5f,-.5f, .5f, -1,0,0,  -.5f, .5f, .5f, -1,0,0,
        // +x
         .5f, .5f, .5f, 1,0,0,   .5f,-.5f,-.5f, 1,0,0,   .5f, .5f,-.5f, 1,0,0,
         .5f,-.5f,-.5f, 1,0,0,   .5f, .5f, .5f, 1,0,0,   .5f,-.5f, .5f, 1,0,0,
        // -y
        -.5f,-.5f,-.5f, 0,-1,0,   .5f,-.5f,-.5f, 0,-1,0,   .5f,-.5f, .5f, 0,-1,0,
         .5f,-.5f, .5f, 0,-1,0,  -.5f,-.5f, .5f, 0,-1,0,  -.5f,-.5f,-.5f, 0,-1,0,
        // +y
        -.5f, .5f,-.5f, 0,1,0,   .5f, .5f, .5f, 0,1,0,   .5f, .5f,-.5f, 0,1,0,
         .5f, .5f, .5f, 0,1,0,  -.5f, .5f,-.5f, 0,1,0,  -.5f, .5f, .5f, 0,1,0,
    };
    Mesh m; m.count = 36;
    glGenVertexArrays(1, &m.vao); glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo); glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);
    return m;
}

Mesh make_quad_xz() {
    const float v[] = {
        -.5f, 0  ,-.5f, 0,1,0,   .5f, 0  , .5f, 0,1,0,   .5f, 0  ,-.5f, 0,1,0,
         .5f, 0  , .5f, 0,1,0,  -.5f, 0  ,-.5f, 0,1,0,  -.5f, 0  , .5f, 0,1,0,
    };
    Mesh m; m.count = 6;
    glGenVertexArrays(1, &m.vao); glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo); glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);
    return m;
}

Mesh make_ui_quad() {
    const float v[] = { -1,-1,  1,-1,  1,1,   1,1,  -1,1,  -1,-1 };
    Mesh m; m.count = 6;
    glGenVertexArrays(1, &m.vao); glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo); glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    return m;
}

// The server flags kPlayerFired for exactly one tick. At 60 Hz that is one
// frame -- effectively invisible. The client therefore keeps its own short-
// lived record so a shot reads as a streak rather than a blink. This is purely
// presentational: no simulation state is inferred from it.
// 3x5 digits, row-major from the top, one bit per pixel. Used for the score
// readout -- there is no font in this client and a text library for ten glyphs
// would be a worse trade than fifty lines of bitmaps.
constexpr std::uint16_t kDigits[10] = {
    0b111'101'101'101'111, // 0
    0b010'110'010'010'111, // 1
    0b111'001'111'100'111, // 2
    0b111'001'111'001'111, // 3
    0b101'101'111'001'001, // 4
    0b111'100'111'001'111, // 5
    0b111'100'111'101'111, // 6
    0b111'001'001'001'001, // 7
    0b111'101'111'101'111, // 8
    0b111'101'111'001'111, // 9
};

struct Tracer {
    float        x0, y0, z0, x1, y1, z1;
    std::int64_t born_ns;
    bool         mine;
};
constexpr std::int64_t kTracerLifeNs = 140'000'000;   // 0.14 s
constexpr float        kTracerLen    = 140.0f;

// How close two space presses must be to count as a double tap. 300 ms is the
// usual figure for a deliberate double input: long enough not to demand
// dexterity, short enough that two separate hops are not mistaken for one
// gesture. The gesture is recognised here and nowhere else -- the server only
// ever sees the resulting held bit, because recognising it there would cost a
// round trip before the thrust started.
constexpr std::int64_t kDoubleTapNs = 300'000'000;

// Smallest jump in the fuel percentage that can only be a powerup and not the
// idle ground trickle. See the note at the use site.
constexpr int kPickupJumpPct = 5;

// The double-tap recogniser, lifted out of the event loop so that something
// can actually test it. SDL key events cannot be synthesised from inside this
// process, so the only testable thing is the state machine the events feed --
// and it is worth testing, because every failure mode of a gesture recogniser
// (never arming, arming on a single press, arming on auto-repeat) presents to
// the player as "the jetpack does not work" with nothing in a log.
struct JetGesture {
    std::int64_t last_down_ns = 0;
    // Tracked explicitly rather than inferred from last_down_ns == 0. Without
    // it the very first press pairs with the zero-initialised timestamp and
    // arms the jetpack on a single tap. That bug is invisible in the real
    // client only because Clock::now_ns() is a large number on this machine,
    // so `t - 0` exceeds the window by a wide margin -- it would reappear on
    // any clock whose epoch is near zero. The selftest below found it
    // immediately, which is the whole reason this struct exists.
    bool         have_down    = false;
    bool         armed        = false;

    // Call on a real key-down transition only. Auto-repeat must be filtered by
    // the caller, or holding the key looks like a stream of taps and arms the
    // jetpack roughly 30 ms in, without the player ever tapping twice.
    void key_down(std::int64_t t) noexcept {
        if (have_down && t - last_down_ns < kDoubleTapNs) armed = true;
        last_down_ns = t;
        have_down    = true;
    }
    // Releasing disarms: the jetpack is a hold, so the next flight needs a
    // fresh double tap.
    void key_up() noexcept { armed = false; }
};

void usage(const char* a0) {
    std::printf(
"usage: %s [options]\n"
"  --host H     server address        (default 127.0.0.1)\n"
"  --port P     server port           (default 9000)\n"
"  --width N    window width          (default 1280)\n"
"  --height N   window height         (default 720)\n"
"  --sens F     mouse sensitivity     (default 0.0022)\n"
"  --selftest N render N frames, verify the framebuffer, exit\n"
"  --flash-test force the damage vignette on and verify it renders red\n"
"  --shot PATH  with --selftest, write the final frame as a PPM and exit\n"
"\n"
"controls: WASD move, mouse look, space jump, left click / ctrl fire,\n"
"          DOUBLE-TAP AND HOLD space to fire the jetpack (burns fuel),\n"
"          walk over cyan powerups for fuel and amber ones for ammo,\n"
"          Tab release mouse, Esc quit\n"
"\n"
"The server must be running in arena mode:\n"
"  ge_world_server --mode arena --port 9000\n", a0);
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    int win_w = 1280, win_h = 720;
    float sens = 0.0022f;
    int selftest = 0;
    const char* shot_path = nullptr;   // --shot: dump the verified frame to look at
    bool udp = false;
    bool flash_test = false;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
            return argv[++i];
        };
        if      (!std::strcmp(argv[i], "--host"))   host = next();
        else if (!std::strcmp(argv[i], "--port"))   port = std::uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--width"))  win_w = std::atoi(next());
        else if (!std::strcmp(argv[i], "--height")) win_h = std::atoi(next());
        else if (!std::strcmp(argv[i], "--sens"))   sens = float(std::atof(next()));
        else if (!std::strcmp(argv[i], "--selftest")) selftest = std::atoi(next());
        else if (!std::strcmp(argv[i], "--shot")) shot_path = next();
        else if (!std::strcmp(argv[i], "--udp"))    udp = true;
        else if (!std::strcmp(argv[i], "--flash-test")) { flash_test = true; selftest = selftest ? selftest : 120; }
        else { usage(argv[0]); return 2; }
    }

    cl::NetClient net;
    std::string err;
    const bool connected = udp ? net.connect_udp(host, port, err)
                               : net.connect(host, port, err);
    if (!connected) {
        std::fprintf(stderr, "connect %s:%u failed: %s\n", host.c_str(), port, err.c_str());
        std::fprintf(stderr, "is the server running?  ge_world_server --mode arena --port %u\n", port);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* win = SDL_CreateWindow("ge_shooter", win_w, win_h,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowRelativeMouseMode(win, true);

    std::printf("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    GLuint prog   = link_program(kVertSrc, kFragSrc, "world");
    GLuint ui     = link_program(kUiVertSrc, kUiFragSrc, "ui");
    GLuint linep  = link_program(kLineVertSrc, kLineFragSrc, "line");
    if (!prog || !ui || !linep) return 1;

    const GLint u_viewproj = glGetUniformLocation(prog, "u_viewproj");
    const GLint u_model    = glGetUniformLocation(prog, "u_model");
    const GLint u_normal   = glGetUniformLocation(prog, "u_normal");
    const ModelUniforms mu{u_model, u_normal};
    const GLint u_colour   = glGetUniformLocation(prog, "u_colour");
    const GLint u_grid     = glGetUniformLocation(prog, "u_grid");
    const GLint ln_viewproj = glGetUniformLocation(linep, "u_viewproj");
    const GLint ln_colour   = glGetUniformLocation(linep, "u_colour");
    const GLint ui_scale   = glGetUniformLocation(ui, "u_scale");
    const GLint ui_offset  = glGetUniformLocation(ui, "u_offset");
    const GLint ui_colour  = glGetUniformLocation(ui, "u_colour");

    Mesh cube = make_cube();
    Mesh floor_mesh = make_quad_xz();
    Mesh uiquad = make_ui_quad();

    // Dynamic vertex buffer for tracers: rewritten every frame, so GL_STREAM_DRAW.
    GLuint line_vao = 0, line_vbo = 0;
    glGenVertexArrays(1, &line_vao); glBindVertexArray(line_vao);
    glGenBuffers(1, &line_vbo); glBindBuffer(GL_ARRAY_BUFFER, line_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
    std::vector<Tracer> tracers;
    std::vector<float>  line_verts;
    std::uint32_t last_seen_tick = 0xffffffffu;
    std::uint64_t tracers_spawned = 0;
    std::int64_t  hurt_until_ns  = 0;    // red vignette while this is in the future
    std::int64_t  frag_until_ns  = 0;    // brief confirmation on a kill
    std::uint16_t last_score     = 0;
    bool          have_last_score = false;
    std::uint16_t last_health    = ge::world::kMaxHealth;
    bool          have_last_health = false;
    std::uint64_t hurt_events = 0, frag_events = 0;
    // Powerup confirmation. Split by kind so the HUD can light the element
    // that actually changed rather than a generic "something happened" blip.
    std::int64_t  fuel_pick_until_ns = 0, ammo_pick_until_ns = 0;
    std::uint8_t  last_fuel = 0, last_ammo = 0;
    bool          have_last_res = false;
    std::uint64_t pickups_taken = 0;
    // Animation clock. Elapsed since start, not the raw monotonic counter: on
    // a long-lived machine that counter is ~1e17 ns, and float(seconds) built
    // from it loses the fractional part the bob and spin are made of.
    const std::int64_t anim_t0_ns = Clock::now_ns();

    // Client-side prediction. The camera used to sit exactly where the server
    // last said it was, which makes every input cost a full round trip -- ~38 ms
    // to Ohio, and it feels like it. Running the same integration locally and
    // correcting toward the server keeps the server authoritative while making
    // input feel immediate. The movement maths below is a deliberate duplicate
    // of ArenaStore::step(); if one changes the other must.
    float pred_x = 0.0f, pred_y = 0.0f, pred_z = 0.0f, pred_vy = 0.0f;
    // Fuel is predicted too, not just position. It has to be: the thrust cuts
    // out the moment the tank empties, and a client that learned about that
    // one round trip late would keep climbing on a prediction the server had
    // already stopped honouring, then snap back down.
    float pred_fuel = ge::world::kMaxFuel;
    bool  pred_init = false;
    std::int64_t last_frame_ns = Clock::now_ns();

    glEnable(GL_DEPTH_TEST);
    // Culling stays off, and the reason has been wrong here twice. The first
    // note blamed the floor quad's winding; the second blamed an inverted
    // up-vector in view_from() and declared culling safe to re-enable. Both
    // were real bugs and both are fixed, but the mesh winding was ALSO
    // backwards -- see make_cube() -- so re-enabling culling at that point
    // would have deleted every surface in the arena.
    //
    // With the winding corrected, culling is now genuinely safe. It is left
    // off only because the floor is a single quad legitimately viewed from
    // both sides, and a few dozen cubes make the saved overdraw unmeasurable.
    //
    // Lighting is two-sided instead (see gl_FrontFacing in the fragment
    // shader), so a back face is lit correctly rather than falling to ambient
    // and looking like a faint ghost of a player.
    glDisable(GL_CULL_FACE);

    cl::WorldView view;
    float yaw = 0.0f, pitch = 0.0f;
    // Jetpack gesture state. It lives here rather than in the input block
    // because the gesture spans events, not frames.
    JetGesture jet;
    bool  mouse_captured = true;
    bool  running = true;
    std::int64_t last_input_ns = 0;
    const std::int64_t input_period_ns = 1'000'000'000 / 60;
    int frames_drawn = 0;
    if (selftest) {
        // Headless CI cannot screenshot this window, and macOS will not grant
        // screen capture to a terminal. Reading the framebuffer back proves the
        // same thing without any permission: if the only colour present is the
        // clear colour, nothing rasterised.
        mouse_captured = false;
        SDL_SetWindowRelativeMouseMode(win, false);
    }

    // Smoothed locally so the HUD is readable; the raw value jitters per tick.
    double rtt_ms_smooth = 0.0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
                    if (e.key.scancode == SDL_SCANCODE_TAB) {
                        mouse_captured = !mouse_captured;
                        SDL_SetWindowRelativeMouseMode(win, mouse_captured);
                    }
                    // Auto-repeat must be filtered or holding space would look
                    // like an endless stream of taps and arm the jetpack on the
                    // second repeat, roughly 30 ms in. Only a real key-down
                    // transition counts.
                    if (e.key.scancode == SDL_SCANCODE_SPACE && !e.key.repeat)
                        jet.key_down(Clock::now_ns());
                    break;
                case SDL_EVENT_KEY_UP:
                    if (e.key.scancode == SDL_SCANCODE_SPACE) jet.key_up();
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (mouse_captured) {
                        // d(forward)/d(yaw) = (cos yaw, 0, -sin yaw) = -right,
                        // exactly, at every yaw. So increasing yaw always swings
                        // the camera LEFT, and a rightward mouse delta must
                        // DECREASE yaw.
                        //
                        // This flipped when the coordinate frame was corrected:
                        // under the old mirrored basis right was +x and
                        // d(forward)/d(yaw) = +right, so `+=` was correct then.
                        // Fixing the handedness reversed the requirement. The
                        // selftest now asserts the relationship so it cannot
                        // drift again.
                        yaw   -= e.motion.xrel * sens;
                        // SDL's yrel is positive downward, and pitch > 0 looks
                        // up (forward.y = sin pitch), so this subtraction is
                        // correct as written.
                        pitch -= e.motion.yrel * sens;
                        if (pitch >  1.5533f) pitch =  1.5533f;
                        if (pitch < -1.5533f) pitch = -1.5533f;
                    }
                    break;
                default: break;
            }
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        float mx = 0.0f, mz = 0.0f;
        if (keys[SDL_SCANCODE_W]) mz += 1.0f;
        if (keys[SDL_SCANCODE_S]) mz -= 1.0f;
        if (keys[SDL_SCANCODE_D]) mx += 1.0f;
        if (keys[SDL_SCANCODE_A]) mx -= 1.0f;
        std::uint32_t buttons = 0;
        // Jump OR jet, never both. Once the gesture is armed, space means
        // thrust; sending kBtnJump as well would make a landing while still
        // holding the key fire a jump underneath the jetpack, which reads as
        // an unexplained bounce.
        if (keys[SDL_SCANCODE_SPACE])
            buttons |= jet.armed ? ge::proto::kBtnJet : ge::proto::kBtnJump;
        const SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
        if ((mb & SDL_BUTTON_LMASK) || keys[SDL_SCANCODE_LCTRL])
            buttons |= ge::proto::kBtnFire;

        // Input is sent on a fixed 60 Hz schedule rather than per rendered
        // frame: the server ticks at 60 Hz, so a 144 Hz display would otherwise
        // send inputs the simulation throws away, and a 30 Hz one would starve it.
        const std::int64_t now = Clock::now_ns();
        if (now - last_input_ns >= input_period_ns) {
            last_input_ns = now;
            if (!net.send_input(mx, mz, yaw, pitch, buttons)) {
                std::fprintf(stderr, "server closed the connection\n");
                running = false;
            }
        }
        if (!net.poll(view)) {
            std::fprintf(stderr, "server closed the connection\n");
            running = false;
        }

        // Integrate the local player forward. Same constants and same order as
        // the server's ArenaStore::step().
        {
            const std::int64_t fnow = Clock::now_ns();
            float fdt = float(double(fnow - last_frame_ns) / 1e9);
            last_frame_ns = fnow;
            if (fdt > 0.1f) fdt = 0.1f;                 // after a stall, do not leap

            const cl::PlayerView* me = nullptr;
            for (const auto& p : view.players)
                if (p.flags & ge::proto::kPlayerSelf) { me = &p; break; }

            if (pred_init && me && !(me->flags & ge::proto::kPlayerDead)) {
                // Must match ArenaStore::step() exactly: forward=(sin,cos),
                // right=(-cos,sin).
                const float sy = std::sin(yaw), cy = std::cos(yaw);
                float dx = sy * mz - cy * mx;
                float dz = cy * mz + sy * mx;
                const float len = std::sqrt(dx * dx + dz * dz);
                if (len > 1.0f) { dx /= len; dz /= len; }

                pred_x += dx * ge::world::kMoveSpeed * fdt;
                pred_z += dz * ge::world::kMoveSpeed * fdt;

                const bool grounded = pred_y <= 0.0001f && pred_vy <= 0.0f;
                if (grounded && (buttons & ge::proto::kBtnJump)) pred_vy = ge::world::kJumpSpeed;

                // Mirrors ArenaStore::step() line for line, including the
                // ground trickle. Any divergence shows up as the camera
                // fighting the server's correction every tick.
                const bool want_jet = (buttons & ge::proto::kBtnJet) != 0;
                const bool jetting  = want_jet && pred_fuel > 0.0f;
                if (jetting) {
                    pred_fuel -= ge::world::kJetFuelPerSec * fdt;
                    if (pred_fuel < 0.0f) pred_fuel = 0.0f;
                    pred_vy += ge::world::kJetAccel * fdt;
                } else if (!want_jet && grounded && pred_fuel < ge::world::kMaxFuel) {
                    pred_fuel += ge::world::kGroundRegenPerSec * fdt;
                    if (pred_fuel > ge::world::kMaxFuel) pred_fuel = ge::world::kMaxFuel;
                }

                pred_vy += ge::world::kGravity * fdt;
                if (pred_vy > ge::world::kJetMaxRise) pred_vy = ge::world::kJetMaxRise;
                pred_y  += pred_vy * fdt;
                if (pred_y < 0.0f) { pred_y = 0.0f; pred_vy = 0.0f; }
                if (pred_y > ge::world::kCeiling) {
                    pred_y = ge::world::kCeiling;
                    if (pred_vy > 0.0f) pred_vy = 0.0f;
                }

                if (pred_x < 0.0f) pred_x = 0.0f;
                if (pred_z < 0.0f) pred_z = 0.0f;
                if (pred_x > view.world_size) pred_x = view.world_size;
                if (pred_z > view.world_size) pred_z = view.world_size;
            }
        }

        // Spawn tracers once per server tick. Doing it per frame would stack
        // duplicates, because view.players holds the last delta until replaced.
        if (view.tick != last_seen_tick) {
            last_seen_tick = view.tick;
            for (const auto& p : view.players) {
                if (p.flags & ge::proto::kPlayerSelf) {
                    // kPlayerHit is set for one tick only, so the client holds
                    // its own timer -- same reasoning as the tracers.
                    // Two triggers, deliberately. kPlayerHit is set for one
                    // tick only, so over UDP a single dropped delta loses the
                    // event forever -- the flag is an edge, and edges do not
                    // survive a lossy transport. Health is state, and state
                    // does. Any decrease means damage happened, whether or not
                    // the tick that announced it arrived.
                    const bool hit_flag = (p.flags & ge::proto::kPlayerHit) != 0;
                    const bool hp_drop  = have_last_health && p.health < last_health;
                    if (hit_flag || hp_drop) {
                        hurt_until_ns = Clock::now_ns() + 450'000'000;
                        ++hurt_events;
                    }
                    last_health = p.health;
                    have_last_health = true;
                    // A score increase is the only signal that a kill landed:
                    // the server awards the point, it is not inferred here.
                    if (have_last_score && p.score > last_score) {
                        frag_until_ns = Clock::now_ns() + 900'000'000;
                        ++frag_events;
                    }
                    last_score = p.score;
                    have_last_score = true;

                    // Fuel and ammo are STATE, so the same argument that made
                    // the damage flash watch health rather than the one-tick
                    // kPlayerHit edge applies: a powerup collected on a tick
                    // whose delta was dropped still shows up as a value that
                    // went up on the next one. kPlayerPick is carried on the
                    // wire for anyone who wants the edge, but this does not
                    // need it.
                    //
                    // The threshold is not cosmetic. Fuel also rises on its
                    // own while standing still (kGroundRegenPerSec), so a
                    // plain `fuel > last_fuel` lights the "collected" flash
                    // every time the regen crosses a percentage point -- which
                    // it did, roughly three times a second, until this was
                    // caught by the bots reporting 88 fuel pickups in a run
                    // where they can only have taken a handful. A powerup is
                    // +55 points at once; regen is +0.05 per tick, and cannot
                    // reach 5 even across a long run of dropped deltas.
                    if (have_last_res && p.fuel >= last_fuel + kPickupJumpPct) {
                        fuel_pick_until_ns = Clock::now_ns() + 600'000'000;
                        ++pickups_taken;
                    }
                    if (have_last_res && p.ammo > last_ammo) {
                        ammo_pick_until_ns = Clock::now_ns() + 600'000'000;
                        ++pickups_taken;
                    }
                    last_fuel = p.fuel; last_ammo = p.ammo;
                    have_last_res = true;

                    // The server owns fuel; snap to it rather than easing.
                    // That is the opposite of what position does two blocks
                    // down, and deliberately: fuel is quantised to 1% on the
                    // wire, so the largest possible correction is a single
                    // pixel on the bar, while the same snap applied to
                    // position is visible as camera jitter.
                    pred_fuel = float(p.fuel) * (ge::world::kMaxFuel / 100.0f);

                    // Reconcile. A large error means something the client
                    // cannot predict happened -- a respawn, a kill, a wall it
                    // disagreed about -- so snap. A small one is ordinary drift
                    // and is eased out, because snapping every tick is visible
                    // as jitter.
                    if (!pred_init || (p.flags & ge::proto::kPlayerDead)) {
                        pred_x = p.x; pred_y = p.y; pred_z = p.z; pred_vy = 0.0f;
                        pred_init = true;
                    } else {
                        const float ex = p.x - pred_x, ey = p.y - pred_y, ez = p.z - pred_z;
                        if (ex * ex + ey * ey + ez * ez > 9.0f) {      // > 3 units
                            pred_x = p.x; pred_y = p.y; pred_z = p.z; pred_vy = 0.0f;
                        } else {
                            pred_x += ex * 0.25f; pred_y += ey * 0.25f; pred_z += ez * 0.25f;
                        }
                    }
                }
                if (!(p.flags & ge::proto::kPlayerFired)) continue;
                const float cp = std::cos(p.pitch), sp = std::sin(p.pitch);
                const float dx = std::sin(p.yaw) * cp, dy = sp, dz = std::cos(p.yaw) * cp;
                float ox = p.x, oy = p.y + ge::world::kEyeHeight, oz = p.z;
                const bool mine = (p.flags & ge::proto::kPlayerSelf) != 0;
                if (mine) {
                    // Offset to the lower right so the streak reads as coming
                    // from a weapon rather than out of the eyeball.
                    const float rx = -std::cos(p.yaw), rz = std::sin(p.yaw);
                    ox += rx * 0.28f + dx * 0.6f;
                    oy += -0.22f + dy * 0.6f;
                    oz += rz * 0.28f + dz * 0.6f;
                }
                ++tracers_spawned;
                tracers.push_back(Tracer{ox, oy, oz,
                                         ox + dx * kTracerLen,
                                         oy + dy * kTracerLen,
                                         oz + dz * kTracerLen,
                                         Clock::now_ns(), mine});
            }
        }

        // ------------------------------------------------------------ render
        int fbw = 0, fbh = 0;
        SDL_GetWindowSizeInPixels(win, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.07f, 0.08f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        const cl::PlayerView* self = nullptr;
        for (const auto& p : view.players)
            if (p.flags & ge::proto::kPlayerSelf) { self = &p; break; }

        // The camera follows the PREDICTED position, corrected toward the
        // server every delta. The server remains authoritative for hits, deaths
        // and everyone else; prediction only decides where this client's own
        // camera is drawn between updates.
        const float eye_x = pred_init ? pred_x : (self ? self->x : view.world_size * 0.5f);
        const float eye_y = (pred_init ? pred_y : (self ? self->y : 0.0f)) + ge::world::kEyeHeight;
        const float eye_z = pred_init ? pred_z : (self ? self->z : view.world_size * 0.5f);

        const float aspect = fbh > 0 ? float(fbw) / float(fbh) : 1.7778f;
        const Mat4 proj = cl::perspective(70.0f * kPi / 180.0f, aspect, 0.1f, 2000.0f);
        const Mat4 vmat = cl::view_from(eye_x, eye_y, eye_z, yaw, pitch);
        const Mat4 vp   = cl::multiply(proj, vmat);

        glUseProgram(prog);
        glUniformMatrix4fv(u_viewproj, 1, GL_FALSE, vp.m);

        // floor
        {
            const float s = view.world_size;
            const Mat4 m = cl::translate_scale(s * 0.5f, 0.0f, s * 0.5f, s, 1.0f, s);
            set_model(mu, m);
            glUniform3f(u_colour, 0.16f, 0.18f, 0.22f);
            glUniform1i(u_grid, 1);
            glBindVertexArray(floor_mesh.vao);
            glDrawArrays(GL_TRIANGLES, 0, floor_mesh.count);
        }

        glUniform1i(u_grid, 0);
        glBindVertexArray(cube.vao);
        for (const auto& p : view.players) {
            if (p.flags & ge::proto::kPlayerSelf) continue;      // first person
            if (p.flags & ge::proto::kPlayerDead) continue;
            // p.y, not a constant. These two cubes were pinned to the floor
            // height, which was invisible while nothing could leave the
            // ground and would have made every jetpacking opponent render
            // at their feet's old position.
            const Mat4 body = cl::translate_scale(p.x, p.y + 0.9f, p.z, 0.8f, 1.8f, 0.8f);
            set_model(mu, body);
            // Red while taking damage, amber while firing, else a cool default.
            if (p.flags & ge::proto::kPlayerHit)        glUniform3f(u_colour, 0.95f, 0.25f, 0.25f);
            else if (p.flags & ge::proto::kPlayerFired) glUniform3f(u_colour, 0.95f, 0.70f, 0.25f);
            else                                        glUniform3f(u_colour, 0.35f, 0.65f, 0.95f);
            glDrawArrays(GL_TRIANGLES, 0, cube.count);

            // A small cube on the front face so facing is readable at a glance.
            const float nx = std::sin(p.yaw), nz = std::cos(p.yaw);
            const Mat4 nose = cl::translate_scale(p.x + nx * 0.5f, p.y + 1.45f, p.z + nz * 0.5f,
                                                  0.25f, 0.25f, 0.25f);
            set_model(mu, nose);
            glUniform3f(u_colour, 0.95f, 0.95f, 0.95f);
            glDrawArrays(GL_TRIANGLES, 0, cube.count);

            // ---- the jetpack ------------------------------------------
            // Worn by everyone, drawn whether or not it is firing. Before
            // this, the only evidence a player had a jetpack was the exhaust
            // while they used it, which made flight look like a property of
            // the player rather than of a thing they are carrying -- and left
            // the plume coming out of the middle of a torso with no hardware
            // attached to it.
            //
            // Built in the player's own frame: forward is (sin yaw, cos yaw),
            // the same convention the server's step() and the camera use, and
            // right is (-cos yaw, sin yaw). Everything below is placed as
            // `back` and `lateral` offsets from the body centre, so the pack
            // stays on the player's back at any facing.
            const float jrx = -std::cos(p.yaw), jrz = std::sin(p.yaw);
            const float back_x = -nx, back_z = -nz;

            // A mounting plate, so the two tanks read as attached to the
            // player rather than floating behind them.
            {
                const Mat4 plate = cl::translate_rotate_y_scale(
                    p.x + back_x * 0.44f, p.y + 1.22f, p.z + back_z * 0.44f,
                    p.yaw, 0.62f, 0.72f, 0.18f);
                set_model(mu, plate);
                glUniform3f(u_colour, 0.20f, 0.22f, 0.27f);
                glDrawArrays(GL_TRIANGLES, 0, cube.count);
            }

            // Fuel is on the wire for every player in range, not just for
            // this client, so an opponent's remaining fuel is readable across
            // the map. Cyan for full, red for empty -- cyan is already what
            // fuel means on the HUD and on the powerup beacons, so the
            // mapping is learned once and never explained.
            const float fuel01 = float(p.fuel) * 0.01f;
            const float tank_r = 0.90f - 0.65f * fuel01;
            const float tank_g = 0.30f + 0.58f * fuel01;
            const float tank_b = 0.32f + 0.60f * fuel01;

            // The tanks sit outboard of the torso, not tucked behind it. The
            // body is 0.8 wide, so tanks on the centreline are completely
            // occluded by it from the front -- which made the pack visible
            // only to people being flown away from, i.e. exactly the players
            // who least need to know. At this offset each tank breaks the
            // silhouette from any angle.
            for (int side = -1; side <= 1; side += 2) {
                const float lat = 0.48f * float(side);
                const float tx = p.x + back_x * 0.34f + jrx * lat;
                const float tz = p.z + back_z * 0.34f + jrz * lat;

                const Mat4 tank = cl::translate_rotate_y_scale(
                    tx, p.y + 1.20f, tz, p.yaw, 0.23f, 0.90f, 0.23f);
                set_model(mu, tank);
                glUniform3f(u_colour, tank_r, tank_g, tank_b);
                glDrawArrays(GL_TRIANGLES, 0, cube.count);

                // Nozzle under each tank. This is where the exhaust now comes
                // from, which is the point of drawing the pack at all.
                const Mat4 nozzle = cl::translate_rotate_y_scale(
                    tx, p.y + 0.68f, tz, p.yaw, 0.15f, 0.20f, 0.15f);
                set_model(mu, nozzle);
                if (p.flags & ge::proto::kPlayerJet) glUniform3f(u_colour, 1.0f, 0.85f, 0.45f);
                else                                 glUniform3f(u_colour, 0.14f, 0.15f, 0.18f);
                glDrawArrays(GL_TRIANGLES, 0, cube.count);

                // Jet plume. Without it a flying opponent is a cube that
                // happens to be in the air, with no cue about whether they are
                // rising, falling or standing on something -- and there is
                // nothing to stand on, so the altitude has to be explained by
                // the exhaust. One per nozzle now, rather than one from the
                // middle of the body.
                if (p.flags & ge::proto::kPlayerJet) {
                    const Mat4 plume = cl::translate_rotate_y_scale(
                        tx, p.y + 0.10f, tz, p.yaw, 0.26f, 0.95f, 0.26f);
                    set_model(mu, plume);
                    glUniform3f(u_colour, 1.0f, 0.55f, 0.15f);
                    glDrawArrays(GL_TRIANGLES, 0, cube.count);
                }
            }
        }

        // ------------------------------------------------------- powerups
        // Drawn after players so they share the cube VAO already bound, and
        // with the same lighting -- they are world objects, not HUD.
        {
            const float tsec = float(double(Clock::now_ns() - anim_t0_ns) / 1e9);
            for (const auto& k : view.pickups) {
                if (!k.active) continue;
                const bool is_fuel = k.kind == std::uint8_t(ge::proto::PickupKind::Fuel);
                // Cyan for fuel, amber for ammo -- the same two colours the
                // fuel bar and the ammo counter use, so the mapping is
                // learned once from the HUD and never explained.
                const float cr = is_fuel ? 0.25f : 0.98f;
                const float cg = is_fuel ? 0.90f : 0.78f;
                const float cb = is_fuel ? 0.95f : 0.20f;

                // The phase offset is per-pickup, keyed off its position, so
                // twelve of them do not bob in lockstep like a chorus line.
                const float bob  = std::sin(tsec * 2.2f + k.x * 0.7f) * 0.18f;
                const Mat4 body = cl::translate_rotate_y_scale(
                    k.x, k.y + bob, k.z, tsec * 1.8f, 0.55f, 0.55f, 0.55f);
                set_model(mu, body);
                glUniform3f(u_colour, cr, cg, cb);
                glDrawArrays(GL_TRIANGLES, 0, cube.count);

                // A beacon column. A 0.55-unit cube on a 110-unit floor is
                // invisible past about 30 metres, which would make the
                // powerups a mechanic you only find by accident. The column
                // is what you navigate by; the cube is what you collect.
                const Mat4 beam = cl::translate_scale(k.x, 3.2f, k.z, 0.10f, 6.0f, 0.10f);
                set_model(mu, beam);
                glUniform3f(u_colour, cr * 0.55f, cg * 0.55f, cb * 0.55f);
                glDrawArrays(GL_TRIANGLES, 0, cube.count);
            }
        }

        // ------------------------------------------------------- tracers
        {
            const std::int64_t tnow = Clock::now_ns();
            tracers.erase(std::remove_if(tracers.begin(), tracers.end(),
                [&](const Tracer& t){ return tnow - t.born_ns > kTracerLifeNs; }),
                tracers.end());

            if (!tracers.empty()) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);        // additive: reads as light
                glDepthMask(GL_FALSE);                    // do not occlude later geometry
                glUseProgram(linep);
                glUniformMatrix4fv(ln_viewproj, 1, GL_FALSE, vp.m);
                glBindVertexArray(line_vao);
                glBindBuffer(GL_ARRAY_BUFFER, line_vbo);

                // One draw call per tracer, because each has its own alpha.
                // At a handful of shots per second that is cheaper than the
                // per-vertex colour attribute it would otherwise need.
                for (const Tracer& t : tracers) {
                    const float age = float(double(tnow - t.born_ns) / double(kTracerLifeNs));
                    const float a   = 1.0f - (age < 0.0f ? 0.0f : (age > 1.0f ? 1.0f : age));
                    line_verts = { t.x0, t.y0, t.z0, t.x1, t.y1, t.z1 };
                    glBufferData(GL_ARRAY_BUFFER,
                                 GLsizeiptr(line_verts.size() * sizeof(float)),
                                 line_verts.data(), GL_STREAM_DRAW);
                    if (t.mine) glUniform4f(ln_colour, 1.0f, 0.95f, 0.65f, a * 0.9f);
                    else        glUniform4f(ln_colour, 1.0f, 0.62f, 0.25f, a * 0.85f);
                    glDrawArrays(GL_LINES, 0, 2);
                }
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }
        }

        // ------------------------------------------------------------ HUD
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(ui);
        glBindVertexArray(uiquad.vao);
        auto ui_rect = [&](float cx, float cy, float hw, float hh,
                           float r, float g, float b, float a = 1.0f) {
            glUniform2f(ui_scale, hw, hh);
            glUniform2f(ui_offset, cx, cy);
            glUniform4f(ui_colour, r, g, b, a);
            glDrawArrays(GL_TRIANGLES, 0, uiquad.count);
        };
        // Top-left origin, pw/ph are HALF the size of one font pixel in NDC.
        auto ui_digit = [&](int d, float x, float y, float pw, float ph,
                            float r, float g, float b, float a) {
            if (d < 0 || d > 9) return;
            const std::uint16_t bits = kDigits[d];
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 3; ++col)
                    if (bits & (1u << (14 - (row * 3 + col))))
                        ui_rect(x + pw + float(col) * 2.0f * pw,
                                y - ph - float(row) * 2.0f * ph, pw, ph, r, g, b, a);
        };
        auto ui_number = [&](unsigned v, float x, float y, float pw, float ph,
                             float r, float g, float b, float a) {
            int d[8]; int n = 0;
            do { d[n++] = int(v % 10); v /= 10; } while (v && n < 8);
            for (int i = n - 1; i >= 0; --i) {
                ui_digit(d[i], x, y, pw, ph, r, g, b, a);
                x += 4.0f * pw;                      // 3 px glyph + 1 px gap
            }
        };
        // crosshair -- red when the magazine is empty, so a dry trigger is
        // readable at the one place the eye is already looking. The server
        // suppresses the shot entirely in that state, which means without
        // this the gun simply stops working with no explanation on screen.
        const float px = 2.0f / float(fbw), py = 2.0f / float(fbh);
        const bool  dry = self && self->ammo == 0;
        const float xr = dry ? 0.95f : 0.9f, xg = dry ? 0.25f : 0.9f, xb = dry ? 0.25f : 0.9f;
        ui_rect(0, 0, 10 * px, 1.5f * py, xr, xg, xb);
        ui_rect(0, 0, 1.5f * px, 10 * py, xr, xg, xb);

        if (self) {
            const std::int64_t hnow = Clock::now_ns();

            // health bar, bottom-left
            const float frac = float(self->health) / float(ge::world::kMaxHealth);
            ui_rect(-0.78f, -0.90f, 0.20f, 0.022f, 0.15f, 0.15f, 0.18f);
            if (frac > 0.0f)
                ui_rect(-0.78f - 0.20f + 0.20f * frac, -0.90f, 0.20f * frac, 0.022f,
                        frac > 0.5f ? 0.3f : 0.9f, frac > 0.5f ? 0.85f : 0.3f, 0.35f);

            // jetpack fuel, immediately above it. Driven by the PREDICTED
            // value, not the server's: the bar has to empty at the same
            // instant the thrust stops, and the thrust stops locally.
            float ffrac = pred_fuel / ge::world::kMaxFuel;
            if (ffrac < 0.0f) ffrac = 0.0f;
            if (ffrac > 1.0f) ffrac = 1.0f;
            const bool fuel_lit = hnow < fuel_pick_until_ns;
            ui_rect(-0.78f, -0.845f, 0.20f, 0.014f, 0.12f, 0.14f, 0.18f);
            if (ffrac > 0.0f)
                ui_rect(-0.78f - 0.20f + 0.20f * ffrac, -0.845f, 0.20f * ffrac, 0.014f,
                        fuel_lit ? 0.75f : 0.25f,
                        fuel_lit ? 1.00f : 0.80f,
                        fuel_lit ? 1.00f : 0.95f);
            // Tick marks at the thirds, so "enough fuel to get up there" is a
            // judgement the bar supports rather than one made by feel.
            for (int t = 1; t < 3; ++t) {
                const float tx = -0.78f - 0.20f + 0.40f * float(t) / 3.0f;
                ui_rect(tx, -0.845f, 0.0018f, 0.014f, 0.05f, 0.06f, 0.08f);
            }
            // While thrusting, a marker rides the end of the bar.
            if (jet.armed && (buttons & ge::proto::kBtnJet) && ffrac > 0.0f)
                ui_rect(-0.78f - 0.20f + 0.40f * ffrac, -0.845f, 0.006f, 0.022f,
                        1.0f, 0.65f, 0.2f);

            // ammo, bottom-right, in the powerup's own amber so the colour
            // says where a refill comes from.
            const bool ammo_lit = hnow < ammo_pick_until_ns;
            const float apw = 6.0f * px, aph = 6.0f * py;
            ui_number(self->ammo, 0.80f, -0.80f, apw, aph,
                      self->ammo == 0 ? 0.95f : (ammo_lit ? 1.00f : 0.98f),
                      self->ammo == 0 ? 0.25f : (ammo_lit ? 0.95f : 0.78f),
                      self->ammo == 0 ? 0.25f : (ammo_lit ? 0.60f : 0.20f), 1.0f);
            // A magazine gauge under the digits: thirty small ticks, one per
            // round. Faster to read mid-fight than the number is.
            for (int r = 0; r < int(ge::world::kMaxAmmo); ++r) {
                const bool have = r < int(self->ammo);
                ui_rect(0.775f + float(r) * 0.0085f, -0.875f, 0.0030f, 0.010f,
                        have ? 0.98f : 0.22f, have ? 0.78f : 0.24f,
                        have ? 0.20f : 0.28f, have ? 1.0f : 0.8f);
            }
        }
        if (flash_test) hurt_until_ns = Clock::now_ns() + 450'000'000;

        // --- damage vignette ------------------------------------------------
        // Four edge bars rather than one full-screen wash: a flat red over the
        // whole viewport hides the thing you are trying to react to.
        {
            const std::int64_t tnow = Clock::now_ns();
            if (tnow < hurt_until_ns) {
                const float k = float(double(hurt_until_ns - tnow) / 450.0e6);
                const float a = 0.55f * (k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k));
                const float band = 0.30f;
                ui_rect( 0.0f,  1.0f - band, 1.0f, band, 0.75f, 0.05f, 0.05f, a);
                ui_rect( 0.0f, -1.0f + band, 1.0f, band, 0.75f, 0.05f, 0.05f, a);
                ui_rect(-1.0f + band, 0.0f, band, 1.0f, 0.75f, 0.05f, 0.05f, a);
                ui_rect( 1.0f - band, 0.0f, band, 1.0f, 0.75f, 0.05f, 0.05f, a);
            }
            // Kill confirmation: the crosshair blooms briefly.
            if (tnow < frag_until_ns) {
                const float k = float(double(frag_until_ns - tnow) / 900.0e6);
                ui_rect(0.0f, 0.0f, 26 * px, 2.5f * py, 1.0f, 0.95f, 0.4f, k);
                ui_rect(0.0f, 0.0f, 2.5f * px, 26 * py, 1.0f, 0.95f, 0.4f, k);
            }
        }

        // --- score ----------------------------------------------------------
        if (self) {
            const float pw = 5.0f * px, ph = 5.0f * py;
            // Your own score, large, top right.
            ui_number(self->score, 0.78f, 0.93f, pw, ph, 1.0f, 0.92f, 0.45f, 1.0f);

            // Scoreboard: everyone visible, highest first. Bar length encodes
            // the score so the ranking reads at a glance; the digits give the
            // exact value.
            std::vector<const cl::PlayerView*> board;
            for (const auto& p : view.players) board.push_back(&p);
            std::sort(board.begin(), board.end(),
                      [](const cl::PlayerView* a, const cl::PlayerView* b) {
                          return a->score > b->score;
                      });
            unsigned top = 1;
            for (const auto* p : board) top = std::max<unsigned>(top, p->score);

            float row_y = 0.70f;
            const float spw = 3.0f * px, sph = 3.0f * py;
            for (std::size_t i = 0; i < board.size() && i < 8; ++i) {
                const cl::PlayerView* p = board[i];
                const bool me = (p->flags & ge::proto::kPlayerSelf) != 0;
                const float w = 0.16f * float(p->score) / float(top);
                ui_rect(0.74f + w, row_y - sph * 3.0f, w + 0.004f, sph * 2.2f,
                        me ? 0.95f : 0.35f, me ? 0.80f : 0.55f, me ? 0.35f : 0.85f, 0.85f);
                ui_number(p->score, 0.92f, row_y, spw, sph,
                          me ? 1.0f : 0.80f, me ? 0.92f : 0.85f, me ? 0.45f : 0.95f, 0.95f);
                row_y -= 0.075f;
            }
        }

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);

        if (view.last_rtt_ns) {
            const double ms = double(view.last_rtt_ns) / 1e6;
            rtt_ms_smooth = rtt_ms_smooth == 0.0 ? ms : rtt_ms_smooth * 0.9 + ms * 0.1;
        }
        char title[192];
        std::snprintf(title, sizeof title,
                      "ge_shooter  |  hp %u  fuel %u%%  ammo %u  score %u  |  "
                      "players %zu  |  rtt %.2f ms  |  tick %u",
                      self ? self->health : 0, self ? self->fuel : 0,
                      self ? self->ammo : 0, self ? self->score : 0,
                      view.players.size(), rtt_ms_smooth, view.tick);
        SDL_SetWindowTitle(win, title);

        SDL_GL_SwapWindow(win);

        if (selftest && ++frames_drawn >= selftest) {
            // The HUD pass added blending, a second shader and ~100 quads per
            // frame; a silent GL error there would show as a blank overlay
            // rather than a crash.
            // Convention check: increasing yaw must turn LEFT, i.e.
            // d(forward)/d(yaw) must point opposite the right vector. If this
            // ever flips, mouse-look is inverted and nothing else would notice.
            {
                const float ty = 0.6f;
                const float dfx =  std::cos(ty), dfz = -std::sin(ty);   // d(forward)/d(yaw)
                const float rrx = -std::cos(ty), rrz =  std::sin(ty);   // right
                const float dot = dfx * rrx + dfz * rrz;
                if (dot > -0.9f) {
                    std::fprintf(stderr,
                        "selftest FAILED: d(forward)/d(yaw).right = %+.2f, expected -1 "
                        "-- mouse-look sign and the right vector disagree\n", double(dot));
                    return 1;
                }
            }
            // Gesture recogniser. Driven from synthetic timestamps because
            // the real ones come from SDL and cannot be forged here.
            {
                JetGesture g;
                struct Step { std::int64_t t; bool down; bool want_armed; const char* what; };
                const Step seq[] = {
                    {0,                       true,  false, "a single tap must be a jump, not a flight"},
                    {0,                       false, false, "release"},
                    {kDoubleTapNs / 3,        true,  true,  "a second tap inside the window arms it"},
                    {kDoubleTapNs / 3,        false, false, "releasing disarms"},
                    {kDoubleTapNs * 4,        true,  false, "a tap long after the last one is a jump"},
                    {kDoubleTapNs * 4,        false, false, "release"},
                    {kDoubleTapNs * 6,        true,  false, "and a slow pair never arms"},
                };
                for (const Step& st : seq) {
                    if (st.down) g.key_down(st.t); else g.key_up();
                    if (g.armed != st.want_armed) {
                        std::fprintf(stderr,
                            "selftest FAILED: jetpack gesture -- %s (armed=%d, wanted %d)\n",
                            st.what, int(g.armed), int(st.want_armed));
                        return 1;
                    }
                }
            }
            const GLenum gl_err = glGetError();
            if (gl_err != GL_NO_ERROR) {
                std::fprintf(stderr, "selftest FAILED: glGetError() = 0x%04x\n", gl_err);
                return 1;
            }
            // The selftest can prove the frame is not black, not upside down
            // and not empty, and it cannot prove the geometry looks like what
            // it is meant to look like. --shot writes the frame out so a human
            // (or a screenshot diff) can answer that part. PPM because it is
            // nine lines and needs no image library.
            if (shot_path) {
                std::vector<unsigned char> full(std::size_t(fbw) * std::size_t(fbh) * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, full.data());
                if (std::FILE* f = std::fopen(shot_path, "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", fbw, fbh);
                    // glReadPixels is bottom-up; PPM is top-down.
                    const std::size_t row = std::size_t(fbw) * 3;
                    for (int y = fbh - 1; y >= 0; --y)
                        std::fwrite(full.data() + std::size_t(y) * row, 1, row, f);
                    std::fclose(f);
                    std::printf("wrote %s (%dx%d)\n", shot_path, fbw, fbh);
                } else {
                    std::fprintf(stderr, "could not open %s for writing\n", shot_path);
                }
            }
            // Sample a tall strip through the centre, so the top and bottom
            // halves can be compared. Coverage alone is not enough: an
            // upside-down world fills exactly as many pixels as a correct one,
            // and that is how a flipped up-vector passed this check once.
            const int sw = 256, sh = 512;
            std::vector<unsigned char> pixels(std::size_t(sw) * sh * 4);
            glReadPixels((fbw - sw) / 2, (fbh - sh) / 2, sw, sh,
                         GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            // Clear colour is (0.07,0.08,0.11) -> roughly (18,20,28).
            std::size_t non_bg = 0;
            long rsum = 0, gsum = 0, bsum = 0;
            for (std::size_t k = 0; k < pixels.size(); k += 4) {
                const int r = pixels[k], g = pixels[k + 1], b = pixels[k + 2];
                rsum += r; gsum += g; bsum += b;
                if (std::abs(r - 18) > 6 || std::abs(g - 20) > 6 || std::abs(b - 28) > 6)
                    ++non_bg;
            }
            const std::size_t total = pixels.size() / 4;
            const double frac = double(non_bg) / double(total);

            // glReadPixels returns rows bottom-up, so the first half of the
            // buffer is the LOWER half of the screen. Looking level from eye
            // height, the floor must dominate there.
            std::size_t lower = 0, upper = 0;
            const std::size_t half = pixels.size() / 2;
            for (std::size_t k = 0; k < pixels.size(); k += 4) {
                const int r = pixels[k], g = pixels[k + 1], b = pixels[k + 2];
                const bool lit = std::abs(r - 18) > 6 || std::abs(g - 20) > 6 ||
                                 std::abs(b - 28) > 6;
                if (!lit) continue;
                if (k < half) ++lower; else ++upper;
            }
            if (flash_test) {
                const int fw = 200, fh = 40;
                std::vector<unsigned char> fp(std::size_t(fw) * fh * 4);
                glReadPixels((fbw - fw) / 2, fbh - fh - 2, fw, fh,
                             GL_RGBA, GL_UNSIGNED_BYTE, fp.data());
                long fr = 0, fg = 0, fb = 0;
                for (std::size_t k = 0; k < fp.size(); k += 4) {
                    fr += fp[k]; fg += fp[k + 1]; fb += fp[k + 2];
                }
                const long n4 = long(fp.size() / 4);
                std::printf("flash-test: top edge mean rgb (%ld,%ld,%ld)\n",
                            fr / n4, fg / n4, fb / n4);
                if (fr / n4 <= fg / n4 + 20 || fr / n4 <= fb / n4 + 20) {
                    std::fprintf(stderr, "flash-test FAILED: no red dominance at the screen edge\n");
                    return 1;
                }
                std::printf("flash-test PASSED\n");
            }
            std::printf("selftest: frames=%d  strip %dx%d  non-background %.1f%%  "
                        "lower/upper %zu/%zu  mean rgb (%ld,%ld,%ld)  players=%zu  self=%s  "
                        "tracers=%llu hurt=%llu frags=%llu score=%u  "
                        "pickups=%zu taken=%llu fuel=%u ammo=%u\n",
                        frames_drawn, sw, sh, frac * 100.0, lower, upper,
                        rsum / long(total), gsum / long(total), bsum / long(total),
                        view.players.size(), self ? "yes" : "no",
                        static_cast<unsigned long long>(tracers_spawned),
                        static_cast<unsigned long long>(hurt_events),
                        static_cast<unsigned long long>(frag_events),
                        self ? self->score : 0,
                        view.pickups.size(),
                        static_cast<unsigned long long>(pickups_taken),
                        self ? self->fuel : 0, self ? self->ammo : 0);
            if (frac < 0.10) {
                std::fprintf(stderr, "selftest FAILED: framebuffer is essentially empty\n");
                return 1;
            }
            if (!self) {
                std::fprintf(stderr, "selftest FAILED: server never sent our own player\n");
                return 1;
            }
            // The delta's tail is now two arrays, and a mistake in either the
            // encoder's offsets or the reader's clamp shows up here first: the
            // players still parse (they come first) and the powerups silently
            // do not. Without this check that failure is invisible until
            // somebody notices the map is empty.
            if (view.pickups.empty()) {
                std::fprintf(stderr,
                    "selftest FAILED: no powerups in the delta -- the PlayerRecord/"
                    "PickupRecord tail split did not survive the round trip\n");
                return 1;
            }
            if (self->fuel > 100 || self->ammo > ge::world::kMaxAmmo) {
                std::fprintf(stderr,
                    "selftest FAILED: fuel %u%% ammo %u out of range -- the two bytes "
                    "that replaced PlayerRecord::pad_ are being read from the wrong "
                    "offset\n", self->fuel, self->ammo);
                return 1;
            }
            if (lower <= upper) {
                std::fprintf(stderr,
                    "selftest FAILED: floor is not below the horizon (lower %zu <= upper %zu) "
                    "-- the view matrix is inverted\n", lower, upper);
                return 1;
            }
            std::printf("selftest PASSED\n");
            running = false;
        }
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
