// ge_shooter_bots -- headless arena clients.
//
// Two jobs. It is the functional test for the arena simulation: bots circle,
// aim at the middle and hold the trigger, so damage, death, respawn and
// scoring all get exercised and reported. And it is the load generator for
// remote runs, because an EC2 instance has no display and ge_shooter needs one.
//
// It speaks the protocol through the same ge::proto codec and the same
// NetClient the graphical client uses, so a bug here is a bug there.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <cstdlib>

#include "ge/core/clock.hpp"
#include "ge/proto/wire.hpp"
#include "ge/world/arena.hpp"
#include "net_client.hpp"

namespace {

using ge::core::Clock;
namespace cl = ge::client;

constexpr float kPi = 3.14159265358979f;

struct Bot {
    cl::NetClient net;
    cl::WorldView view;
    float         phase = 0.0f;
    std::uint32_t last_health = ge::world::kMaxHealth;
    std::uint32_t last_tick   = 0xffffffffu;   // so stats count deltas, not polls
    std::uint64_t hits_taken = 0, deaths = 0, shots_seen = 0;
    std::uint8_t  last_fuel = 0, last_ammo = 0;
    bool          have_res  = false;
    std::uint64_t fuel_taken = 0, ammo_taken = 0, dry_ticks = 0;

    // Timestamps of inputs sent but not yet reflected in any snapshot we have
    // received. Pushed in send order, so it is sorted and can be drained from
    // the front. This is what makes per-INPUT latency accounting possible:
    // without it, an input whose snapshots were all lost simply vanishes from
    // the sample instead of counting as a slow one.
    std::vector<std::uint64_t> outstanding;
    std::size_t                out_cursor    = 0;
    std::uint64_t              last_echo_seen = 0;
};

void usage(const char* a0) {
    std::printf(
"usage: %s [options]\n"
"  --host H       server            (default 127.0.0.1)\n"
"  --port P       port              (default 9000)\n"
"  --bots N       concurrent bots   (default 8)\n"
"  --seconds S    run length        (default 10)\n"
"  --no-fire      move but never shoot\n"
"  --json         emit one JSON line instead of a table\n"
"  --drop-pct N   discard N%% of received deltas, to test loss tolerance\n"
"  --send-drop-pct N  discard N%% of SENT inputs, to test the server's\n"
"                 sequence-gap counter against a known loss rate\n"
"  --aim-error D  add +/-D degrees of random aim error (default 0 = perfect)\n"
"  --fire-duty P  hold the trigger only P%% of the time (default 100)\n"
"  --no-scavenge  never walk toward powerups (bots will run dry and stay dry)\n", a0);
}

int cmp_u64(const void* a, const void* b) {
    const std::uint64_t x = *static_cast<const std::uint64_t*>(a), y = *static_cast<const std::uint64_t*>(b);
    return (x > y) - (x < y);
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    int nbots = 8, seconds = 10;
    bool fire = true, json = false, udp = false;
    int drop_pct = 0;
    int send_drop_pct = 0;
    double aim_error_deg = 0.0;
    int fire_duty = 100;
    bool scavenge = true;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
            return argv[++i];
        };
        if      (!std::strcmp(argv[i], "--host"))    host = next();
        else if (!std::strcmp(argv[i], "--port"))    port = std::uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--bots"))    nbots = std::atoi(next());
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atoi(next());
        else if (!std::strcmp(argv[i], "--no-fire")) fire = false;
        else if (!std::strcmp(argv[i], "--json"))    json = true;
        else if (!std::strcmp(argv[i], "--udp"))     udp = true;
        else if (!std::strcmp(argv[i], "--drop-pct")) drop_pct = std::atoi(next());
        else if (!std::strcmp(argv[i], "--send-drop-pct")) send_drop_pct = std::atoi(next());
        else if (!std::strcmp(argv[i], "--aim-error")) aim_error_deg = std::atof(next());
        else if (!std::strcmp(argv[i], "--fire-duty")) fire_duty = std::atoi(next());
        else if (!std::strcmp(argv[i], "--no-scavenge")) scavenge = false;
        else { usage(argv[0]); return 2; }
    }

    // Named, because `std::vector<Bot> bots(std::size_t(nbots));` is a function
    // declaration -- the most vexing parse, and the compiler's error about
    // "subscript of pointer to function type" is a long way from the cause.
    const std::size_t bot_count = std::size_t(nbots);
    std::vector<Bot> bots(bot_count);
    for (std::size_t i = 0; i < bot_count; ++i) {
        std::string err;
        const bool ok = udp ? bots[i].net.connect_udp(host, port, err)
                            : bots[i].net.connect(host, port, err);
        if (!ok) {
            std::fprintf(stderr, "bot %zu connect failed: %s\n", i, err.c_str());
            return 1;
        }
        bots[i].phase = float(i) * (2.0f * kPi / float(nbots));
        // Per-bot, so injected loss is N independent draws rather than one
        // draw replayed N times. Derived from the index, not the clock, so a
        // run with the same arguments drops the same datagrams.
        bots[i].net.set_loss_seed(std::uint64_t(i) + 1);
    }

    std::vector<std::uint64_t> pongs;
    std::vector<std::uint64_t> rtts;            // one sample per snapshot received
    std::vector<std::uint64_t> rtts_per_input;  // one sample per input sent
    rtts.reserve(std::size_t(nbots) * std::size_t(seconds) * 64);
    rtts_per_input.reserve(std::size_t(nbots) * std::size_t(seconds) * 64);

    const std::int64_t t_end = Clock::now_ns() + std::int64_t(seconds) * 1'000'000'000LL;
    const std::int64_t period = 1'000'000'000 / 60;
    std::int64_t next_send = Clock::now_ns();
    std::uint64_t sent = 0, deltas = 0;

    while (Clock::now_ns() < t_end) {
        const std::int64_t now = Clock::now_ns();
        if (now >= next_send) {
            next_send += period;
            const float t = float(double(now) / 1e9);
            for (auto& b : bots) {
                // Circle the arena and sweep the aim across the middle, so
                // sooner or later every bot has another in its sights.
                // Aim at the nearest visible opponent. A blind sweep landed
                // roughly one hit per ten seconds, which is too weak to call a
                // test of hitscan; aiming makes damage, death, respawn and
                // scoring all fire reliably.
                float yaw   = b.phase + t * 0.9f;
                float pitch = 0.10f * std::sin(t * 0.7f + b.phase);
                const cl::PlayerView* me = nullptr;
                for (const auto& p : b.view.players)
                    if (p.flags & ge::proto::kPlayerSelf) { me = &p; break; }
                if (me) {
                    const cl::PlayerView* target = nullptr;
                    float best = 1e30f;
                    for (const auto& p : b.view.players) {
                        if (p.flags & (ge::proto::kPlayerSelf | ge::proto::kPlayerDead)) continue;
                        const float dx = p.x - me->x, dz = p.z - me->z;
                        const float d2 = dx * dx + dz * dz;
                        if (d2 < best) { best = d2; target = &p; }
                    }
                    if (target) {
                        const float dx = target->x - me->x;
                        const float dz = target->z - me->z;
                        const float dy = (target->y + ge::world::kTorsoHeight)
                                       - (me->y + ge::world::kEyeHeight);
                        yaw   = std::atan2(dx, dz);          // matches the server: forward = +z rotated by yaw
                        pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
                        // Perfect aim is what the functional test wants and the
                        // worst thing to put a human in front of: five bots at
                        // 8.3 shots/s and 25 damage is ~1040 dps against 100 hp,
                        // so a player dies in 0.1 s and spends the 3 s respawn
                        // timer watching. Error is opt-in so tests keep their
                        // determinism.
                        if (aim_error_deg > 0.0) {
                            const double k = aim_error_deg * 3.14159265 / 180.0;
                            yaw   += float(((double(rand()) / RAND_MAX) * 2.0 - 1.0) * k);
                            pitch += float(((double(rand()) / RAND_MAX) * 2.0 - 1.0) * k);
                        }
                    }
                }
                // Default wander.
                float mx = std::sin(t * 0.5f + b.phase);
                float mz = std::cos(t * 0.5f + b.phase);

                // Scavenging. Bots that never collect anything run dry about
                // four seconds in and the arena goes quiet for the rest of the
                // run, which makes a playtest of the ammo system useless --
                // the thing you want to see is opponents reloading, not
                // opponents permanently disarmed. Low on either resource, head
                // for the nearest powerup of the kind that is short.
                if (scavenge && me) {
                    const bool want_ammo = me->ammo <= 8;
                    const bool want_fuel = me->fuel <= 30;
                    if (want_ammo || want_fuel) {
                        const std::uint8_t want = want_ammo
                            ? std::uint8_t(ge::proto::PickupKind::Ammo)
                            : std::uint8_t(ge::proto::PickupKind::Fuel);
                        const cl::PickupView* goal = nullptr;
                        float best = 1e30f;
                        for (const auto& k : b.view.pickups) {
                            if (!k.active || k.kind != want) continue;
                            const float dx = k.x - me->x, dz = k.z - me->z;
                            const float d2 = dx * dx + dz * dz;
                            if (d2 < best) { best = d2; goal = &k; }
                        }
                        if (goal) {
                            // The aim is pointed at an opponent, so the walk
                            // direction has to be expressed in the bot's own
                            // frame rather than the world's. Same basis the
                            // server integrates with: forward = (sin yaw, cos
                            // yaw), right = (-cos yaw, sin yaw).
                            const float wx = goal->x - me->x, wz = goal->z - me->z;
                            const float len = std::sqrt(wx * wx + wz * wz);
                            if (len > 0.01f) {
                                const float ux = wx / len, uz = wz / len;
                                const float sy = std::sin(yaw), cy = std::cos(yaw);
                                mz =  ux * sy + uz * cy;
                                mx = -ux * cy + uz * sy;
                            }
                        }
                    }
                }

                std::uint32_t btn = 0u;
                if (fire && (fire_duty >= 100 ||
                             double(std::sin(t * 0.8f + b.phase * 2.0f)) * 50.0 + 50.0
                                 < double(fire_duty)))
                    btn = ge::proto::kBtnFire;
                // Jet in short bursts when there is fuel to spare, otherwise
                // hop. Exclusive, matching what the real client sends: the
                // server would honour both in one tick and the resulting
                // jump-assisted launch is not a state a player can reach.
                if (me && me->fuel > 35 && std::sin(t * 0.6f + b.phase * 3.0f) > 0.80f)
                    btn |= ge::proto::kBtnJet;
                else if (std::sin(t * 1.7f + b.phase) > 0.9f)
                    btn |= ge::proto::kBtnJump;
                if (!b.net.send_input(mx, mz, yaw, pitch, btn)) {
                    std::fprintf(stderr, "a bot lost its connection\n");
                    return 1;
                }
                b.outstanding.push_back(b.net.last_input_ts());
                if (!b.net.send_ping()) {
                    std::fprintf(stderr, "a bot lost its connection\n");
                    return 1;
                }
                ++sent;
            }
        }

        // Yield when there is nothing to do. Without this the loop spins a
        // whole core flat out, and on a small box it competes with the server
        // it is supposed to be loading -- which measured as 316 ms of ping
        // latency on a 2-vCPU instance, entirely self-inflicted. 200 us is far
        // below the 16.7 ms tick, so nothing is delayed by it.
        bool any_new = false;

        for (auto& b : bots) {
            const std::size_t before = b.view.players.size();
            (void)before;
            const std::uint64_t prev_rtt  = b.view.last_rtt_ns;
            const std::uint64_t prev_pong = b.view.last_pong_ns;
            // Induced loss. Dropping AFTER receive rather than blocking the
            // socket keeps the test at the application layer, where it is
            // honest about what it simulates: the datagram arrived and we chose
            // to ignore it, which is indistinguishable from downlink loss as far
            // as the game state machine is concerned.
            b.net.set_drop_pct(drop_pct);
            b.net.set_send_drop_pct(send_drop_pct);
            if (!b.net.poll(b.view)) {
                std::fprintf(stderr, "a bot lost its connection\n");
                return 1;
            }
            if (b.view.last_pong_ns && b.view.last_pong_ns != prev_pong)
                pongs.push_back(b.view.last_pong_ns);
            if (b.view.last_rtt_ns && b.view.last_rtt_ns != prev_rtt) {
                rtts.push_back(b.view.last_rtt_ns);
                ++deltas;
                any_new = true;
            }
            // Per-input accounting. The snapshot just received echoes the
            // timestamp of the last input the server applied for us, so EVERY
            // outstanding input at or before that timestamp has now been
            // answered -- by this snapshot, at this moment. Recording a sample
            // for each of them, rather than one sample for the snapshot, is
            // what removes the survivorship bias: inputs whose own snapshots
            // were dropped are charged the full wait they actually incurred
            // instead of disappearing from the distribution.
            //
            // This is the coordinated-omission correction, applied at the
            // layer where it is meaningful for a game: the player pressed a
            // key at `ts` and first saw its effect now.
            if (b.view.last_echo_ts_ns && b.view.last_echo_ts_ns != b.last_echo_seen) {
                b.last_echo_seen = b.view.last_echo_ts_ns;
                const std::uint64_t now_ns = std::uint64_t(Clock::now_ns());
                while (b.out_cursor < b.outstanding.size() &&
                       b.outstanding[b.out_cursor] <= b.view.last_echo_ts_ns) {
                    const std::uint64_t ts = b.outstanding[b.out_cursor++];
                    rtts_per_input.push_back(now_ns >= ts ? now_ns - ts : 0);
                }
                if (b.out_cursor > 4096) {   // compact, keeps memory flat
                    b.outstanding.erase(b.outstanding.begin(),
                                        b.outstanding.begin() + std::ptrdiff_t(b.out_cursor));
                    b.out_cursor = 0;
                }
            }
            // poll() runs far faster than deltas arrive, and view.players holds
            // the previous delta until a new one replaces it. Counting on every
            // poll therefore counts the same tick thousands of times -- which is
            // how a 10-second run reported 32 million shots. Gate on the tick.
            if (b.view.tick == b.last_tick) continue;
            b.last_tick = b.view.tick;
            for (const auto& p : b.view.players) {
                if (!(p.flags & ge::proto::kPlayerSelf)) {
                    if (p.flags & ge::proto::kPlayerFired) ++b.shots_seen;
                    continue;
                }
                // Resource deltas, counted the same way damage is: by
                // watching the value, not by trusting a one-tick flag that a
                // dropped datagram would erase.
                //
                // Fuel needs a threshold because it also rises on its own
                // while a bot stands still (kGroundRegenPerSec). Counting any
                // increase reported 88 "fuel pickups" in a 20-second run,
                // which is how this was noticed. A powerup is +55 points at
                // once; the trickle is +0.05 per tick. Ammo has no such
                // source, so any increase there is genuinely a pickup.
                if (b.have_res) {
                    if (p.fuel >= b.last_fuel + 5) ++b.fuel_taken;
                    if (p.ammo > b.last_ammo) ++b.ammo_taken;
                }
                if (p.ammo == 0) ++b.dry_ticks;
                b.last_fuel = p.fuel; b.last_ammo = p.ammo; b.have_res = true;
                if (p.health < b.last_health) ++b.hits_taken;
                if ((p.flags & ge::proto::kPlayerDead) &&
                    !(b.last_health == 0)) ++b.deaths;
                b.last_health = p.health;
            }
        }

        if (!any_new && Clock::now_ns() + 500'000 < next_send)
            ::usleep(200);
    }

    std::uint64_t hits = 0, deaths = 0, shots = 0, best_score = 0;
    std::uint64_t unanswered = 0;
    std::uint64_t fuel_taken = 0, ammo_taken = 0, dry_ticks = 0;
    std::size_t max_seen = 0, max_pickups = 0;
    for (auto& b : bots) {
        fuel_taken += b.fuel_taken; ammo_taken += b.ammo_taken; dry_ticks += b.dry_ticks;
        max_pickups = std::max(max_pickups, b.view.pickups.size());
        hits += b.hits_taken; deaths += b.deaths; shots += b.shots_seen;
        max_seen = std::max(max_seen, b.view.players.size());
        for (const auto& p : b.view.players)
            if (p.flags & ge::proto::kPlayerSelf) best_score = std::max<std::uint64_t>(best_score, p.score);
        // Inputs still outstanding when the run ended: never reflected in any
        // snapshot we received. Reported, not silently dropped.
        unanswered += b.outstanding.size() - b.out_cursor;
    }

    std::qsort(rtts.data(), rtts.size(), sizeof(std::uint64_t), cmp_u64);
    std::qsort(rtts_per_input.data(), rtts_per_input.size(), sizeof(std::uint64_t), cmp_u64);
    std::qsort(pongs.data(), pongs.size(), sizeof(std::uint64_t), cmp_u64);
    // Exact quantiles from the sorted sample, not a binned histogram. At these
    // sample counts (a few million 8-byte values, tens of MB) exactness is
    // affordable, and it avoids having to defend a bucket resolution.
    auto q_of = [](const std::vector<std::uint64_t>& v, double q) -> double {
        if (v.empty()) return 0.0;
        return double(v[std::size_t(double(v.size() - 1) * q)]) / 1000.0;   // us
    };
    auto pct  = [&](double q) { return q_of(rtts, q); };
    auto ipct = [&](double q) { return q_of(rtts_per_input, q); };
    auto ppct = [&](double q) { return q_of(pongs, q); };

    if (json) {
        std::printf("{\"bots\":%d,\"seconds\":%d,\"inputs_sent\":%llu,\"deltas\":%llu,"
                    "\"pong_p50_us\":%.2f,\"pong_p90_us\":%.2f,\"pong_p99_us\":%.2f,"
                    "\"rtt_samples\":%zu,\"p50_us\":%.2f,\"p90_us\":%.2f,\"p99_us\":%.2f,"
                    "\"p999_us\":%.2f,\"max_us\":%.2f,"
                    "\"in_samples\":%zu,\"in_p50_us\":%.2f,\"in_p90_us\":%.2f,"
                    "\"in_p99_us\":%.2f,\"in_p999_us\":%.2f,\"in_p9999_us\":%.2f,"
                    "\"in_max_us\":%.2f,\"unanswered\":%llu,"
                    "\"hits_taken\":%llu,\"deaths\":%llu,"
                    "\"shots_seen\":%llu,\"max_visible\":%zu,\"best_score\":%llu,"
                    "\"pickups_visible\":%zu,\"fuel_pickups\":%llu,\"ammo_pickups\":%llu,"
                    "\"dry_ticks\":%llu}\n",
                    nbots, seconds, static_cast<unsigned long long>(sent), static_cast<unsigned long long>(deltas),
                    ppct(0.50), ppct(0.90), ppct(0.99), rtts.size(), pct(0.50), pct(0.90), pct(0.99), pct(0.999),
                    rtts.empty() ? 0.0 : double(rtts.back()) / 1000.0,
                    rtts_per_input.size(), ipct(0.50), ipct(0.90), ipct(0.99), ipct(0.999),
                    ipct(0.9999),
                    rtts_per_input.empty() ? 0.0 : double(rtts_per_input.back()) / 1000.0,
                    static_cast<unsigned long long>(unanswered),
                    static_cast<unsigned long long>(hits), static_cast<unsigned long long>(deaths),
                    static_cast<unsigned long long>(shots), max_seen,
                    static_cast<unsigned long long>(best_score), max_pickups,
                    static_cast<unsigned long long>(fuel_taken),
                    static_cast<unsigned long long>(ammo_taken),
                    static_cast<unsigned long long>(dry_ticks));
    } else {
        if (drop_pct) std::printf("\n  [injected %d%% delta loss]\n", drop_pct);
        if (send_drop_pct) std::printf("  [injected %d%% input loss]\n", send_drop_pct);
        std::printf("\nbots %d for %ds -> %llu inputs, %llu deltas\n",
                    nbots, seconds, static_cast<unsigned long long>(sent), static_cast<unsigned long long>(deltas));
        // This is input -> the delta that reflects it, so it necessarily
        // includes waiting for the next 60 Hz tick (0-16.7 ms, ~8 ms mean).
        // It is the number a player feels. It is NOT the transport latency --
        // for that see the Ping/Pong figure, which the server answers on the
        // I/O thread without the simulation ever seeing it.
        std::printf("  input->state       p50 %8.2f ms   p90 %8.2f ms   p99 %8.2f ms   max %8.2f ms\n",
                    pct(0.50) / 1000.0, pct(0.90) / 1000.0, pct(0.99) / 1000.0,
                    rtts.empty() ? 0.0 : double(rtts.back()) / 1e6);
        std::printf("                     (per snapshot received -- n=%zu; biased: an input whose\n"
                    "                      snapshots were all lost contributes no sample at all)\n",
                    rtts.size());
        // The honest one. Same run, same clock, different denominator.
        std::printf("  per-input latency  p50 %8.2f ms   p90 %8.2f ms   p99 %8.2f ms\n"
                    "                     p99.9 %6.2f ms   p99.99 %6.2f ms  max %8.2f ms\n",
                    ipct(0.50) / 1000.0, ipct(0.90) / 1000.0, ipct(0.99) / 1000.0,
                    ipct(0.999) / 1000.0, ipct(0.9999) / 1000.0,
                    rtts_per_input.empty() ? 0.0 : double(rtts_per_input.back()) / 1e6);
        std::printf("                     (per input sent -- n=%zu, %llu never answered = %.3f%%)\n",
                    rtts_per_input.size(), static_cast<unsigned long long>(unanswered),
                    sent ? 100.0 * double(unanswered) / double(sent) : 0.0);
        std::printf("                     (includes up to one 60 Hz tick of quantisation)\n");
        std::printf("  transport RTT      p50 %8.2f us   p90 %8.2f us   p99 %8.2f us   (ping/pong, no sim)\n",
                    ppct(0.50), ppct(0.90), ppct(0.99));
        std::printf("  gameplay           hits taken %llu, deaths %llu, shots observed %llu\n",
                    static_cast<unsigned long long>(hits), static_cast<unsigned long long>(deaths),
                    static_cast<unsigned long long>(shots));
        std::printf("  visibility         up to %zu players in one delta, best score %llu\n",
                    max_seen, static_cast<unsigned long long>(best_score));
        std::printf("  powerups           %zu on the map; collected %llu fuel, %llu ammo; "
                    "%llu ticks spent dry\n",
                    max_pickups, static_cast<unsigned long long>(fuel_taken),
                    static_cast<unsigned long long>(ammo_taken),
                    static_cast<unsigned long long>(dry_ticks));
        // hits/deaths count damage *received* by the bots. With a single bot
        // against server-side opponents it can legitimately be zero while the
        // bot racks up kills, so the warning has to consider the score too --
        // it fired a false alarm on exactly that case.
        if (fire && hits == 0 && best_score == 0 && max_seen > 1)
            std::printf("  WARNING: no damage dealt or taken -- hitscan or AOI is broken\n");
    }
    return 0;
}
