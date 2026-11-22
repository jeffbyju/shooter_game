// ge_bench -- component microbenchmarks.
//
// Methodology: each case runs a batch of N iterations, timed as a whole, and
// the batch is repeated R times. The reported figure is the *minimum* batch,
// because measurement noise on a multitasking OS is strictly additive -- an
// interrupt can only make a run slower, never faster -- so the minimum is the
// closest estimate of the true cost. The spread between min and median is
// printed alongside it, so a case whose cost is genuinely variable cannot hide
// behind a flattering minimum.
//
// Batching also sidesteps the clock: on Apple Silicon the monotonic clock ticks
// at ~41ns, which is several times the cost of the operations measured here.
// Timing 100k iterations at once makes that granularity irrelevant.
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ge/alloc/arena.hpp"
#include "ge/alloc/counters.hpp"
#include "ge/alloc/pool.hpp"
#include "ge/container/slot_map.hpp"
#include "ge/container/spsc_byte_ring.hpp"
#include "ge/container/spsc_ring.hpp"
#include "ge/core/bits.hpp"
#include "ge/core/clock.hpp"
#include "ge/metrics/histogram.hpp"
#include "ge/proto/codec.hpp"
#include "ge/world/grid.hpp"
#include "ge/world/shard.hpp"

using namespace ge;
using core::Clock;

namespace {

struct Result {
    std::string   name;
    double        ns_per_op   = 0;   // from the best batch
    double        ns_median   = 0;
    double        ops_per_sec = 0;
    std::uint64_t allocs      = 0;
    std::string   note;
};

std::vector<Result> g_results;

template <typename Fn>
void bench(const char* name, std::uint64_t iters, Fn&& fn, const char* note = "") {
    constexpr int kRepeats = 7;
    fn();                                            // warm caches and branch predictors

    std::vector<double> batches;
    batches.reserve(kRepeats);
    alloc::AllocScope scope;
    for (int r = 0; r < kRepeats; ++r) {
        const std::int64_t t0 = Clock::now_ns();
        fn();
        const std::int64_t t1 = Clock::now_ns();
        batches.push_back(double(t1 - t0) / double(iters));
    }
    const auto allocs = scope.delta().allocs;
    std::sort(batches.begin(), batches.end());

    Result res;
    res.name        = name;
    res.ns_per_op   = batches.front();
    res.ns_median   = batches[batches.size() / 2];
    res.ops_per_sec = res.ns_per_op > 0 ? 1e9 / res.ns_per_op : 0;
    res.allocs      = allocs / kRepeats;
    res.note        = note;
    g_results.push_back(res);

    std::printf("  %-38s %9.2f ns  %8.2f ns  %11s  %6" PRIu64 "  %s\n",
                name, res.ns_per_op, res.ns_median,
                res.ops_per_sec >= 1e6
                    ? (std::to_string(std::int64_t(res.ops_per_sec / 1e6)) + " M/s").c_str()
                    : (std::to_string(std::int64_t(res.ops_per_sec / 1e3)) + " K/s").c_str(),
                res.allocs, note);
    std::fflush(stdout);
}

void section(const char* title) {
    std::printf("\n\033[1m%s\033[0m\n", title);
    std::printf("  %-38s %12s %11s %11s %7s\n", "case", "best", "median", "rate", "allocs");
    std::printf("  ------------------------------------------------------------------------------------\n");
}

// ---------------------------------------------------------------- protocol
alignas(8) std::byte g_frame[8192];

void bench_protocol() {
    section("PROTOCOL  (encode / decode / dispatch)");

    constexpr std::uint64_t kN = 2'000'000;
    bench("encode Ping (32B frame)", kN, [&] {
        for (std::uint64_t i = 0; i < kN; ++i) {
            core::compiler_barrier();
            const proto::Ping p{i, i ^ 0xFFFF};
            const std::size_t n = proto::encode(g_frame, p, 1, std::uint32_t(i));
            core::do_not_optimize(n);
        }
    }, "struct -> wire, no serialisation step");

    // A compiler barrier at the top of each iteration is mandatory here: the
    // buffer is a file-scope array with a loop-invariant decode, so without it
    // clang hoists the entire body out and the case measures 0.00 ns. Any
    // microbenchmark reporting a sub-cycle cost for real work is measuring the
    // optimiser, not the code.
    proto::encode(g_frame, proto::Ping{1, 2}, 1, 1);
    bench("decode + type-checked view", kN, [&] {
        std::uint64_t acc = 0;
        for (std::uint64_t i = 0; i < kN; ++i) {
            core::compiler_barrier();
            proto::FrameReader r(std::span<const std::byte>(g_frame, 32));
            proto::FrameView f;
            r.next(f);
            acc += f.as<proto::Ping>()->client_ts_ns;
        }
        core::do_not_optimize(acc);
    }, "zero copy: pointer into the buffer");

    bench("decode + visit dispatch", kN, [&] {
        std::uint64_t acc = 0;
        for (std::uint64_t i = 0; i < kN; ++i) {
            core::compiler_barrier();
            proto::FrameReader r(std::span<const std::byte>(g_frame, 32));
            proto::FrameView f;
            r.next(f);
            proto::visit(f, [&](const auto& m, auto...) {
                if constexpr (std::is_same_v<std::decay_t<decltype(m)>, proto::Ping>)
                    acc += m.client_ts_ns;
            });
        }
        core::do_not_optimize(acc);
    }, "jump table, no virtual call");

    // A full area-of-interest snake delta: the largest frame the protocol emits.
    // Note this is now a *nested* variable-length structure -- a run of records,
    // each followed by its own point array -- so the encoder writes two levels
    // of variable-length data straight into the destination.
    constexpr std::uint64_t kD = 100'000;
    constexpr std::uint16_t kSnakes = 16, kSegs = 32;
    bench("encode SnakeDelta (16 x 32 pts)", kD, [&] {
        for (std::uint64_t i = 0; i < kD; ++i) {
            core::compiler_barrier();
            proto::SnakeDeltaWriter w(g_frame, 1, std::uint32_t(i));
            for (std::uint16_t s = 0; s < kSnakes; ++s) {
                auto pts = w.begin_snake(kSegs);
                if (pts.empty()) break;
                for (std::uint16_t g = 0; g < kSegs; ++g)
                    pts[g] = proto::Point2{float(g), float(s)};
                proto::SnakeRecord rec{};
                rec.id = s;
                w.commit_snake(rec, kSegs);
            }
            core::do_not_optimize(w.finish(proto::SnakeDeltaHeader{}));
        }
    }, "two nested levels, written in place");

    {
        proto::SnakeDeltaWriter w(g_frame, 1, 1);
        for (std::uint16_t s = 0; s < kSnakes; ++s) {
            auto pts = w.begin_snake(kSegs);
            for (std::uint16_t g = 0; g < kSegs; ++g) pts[g] = proto::Point2{float(g), float(s)};
            proto::SnakeRecord rec{};
            rec.id = s;
            w.commit_snake(rec, kSegs);
        }
        const std::size_t n = w.finish(proto::SnakeDeltaHeader{});
        bench("decode SnakeDelta + walk all points", kD, [&] {
            float acc = 0;
            for (std::uint64_t i = 0; i < kD; ++i) {
                core::compiler_barrier();
                proto::FrameReader r(std::span<const std::byte>(g_frame, n));
                proto::FrameView f;
                r.next(f);
                proto::SnakeDeltaView v(f);
                const proto::SnakeRecord* rec = nullptr;
                std::span<const proto::Point2> pts;
                while (v.next(rec, pts)) for (const auto& pt : pts) acc += pt.x;
            }
            core::do_not_optimize(acc);
        }, "bounds-checked walk; points alias the buffer");
    }
}

// --------------------------------------------------------------- containers
// ---------------------------------------------------------------- false sharing
//
// Per-thread counters exist so that threads do not contend. That only holds if
// they land on different cache lines. The arena server's Stats was 120 bytes in
// a vector, so with 64-byte lines thread k's tail shared a line with thread
// k+1's head, and every increment invalidated a neighbour -- in the hottest
// loop in the server. This measures the cost of that mistake directly: the
// identical loop, run on a shared layout and a padded one.
void bench_false_sharing() {
    constexpr std::uint64_t kIters = 20'000'000;
    const unsigned nthreads =
        std::min(4u, std::max(2u, std::thread::hardware_concurrency()));

    struct Unpadded { std::uint64_t n = 0; };
    struct alignas(128) Padded { std::uint64_t n = 0; };  // 128: Apple Silicon's line

    auto run = [&](auto* slots) {
        std::atomic<bool> go{false};
        std::vector<std::thread> ts;
        ts.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t)
            ts.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) { }
                for (std::uint64_t i = 0; i < kIters; ++i) {
                    slots[t].n += 1;                 // only ever its OWN counter
                    // Without this the compiler collapses the loop to one add
                    // and the benchmark measures nothing. The server's own
                    // counters survive because syscalls sit between them.
                    asm volatile("" ::: "memory");
                }
            });
        const auto t0 = ge::core::Clock::now_ns();
        go.store(true, std::memory_order_release);
        for (auto& th : ts) th.join();
        return double(ge::core::Clock::now_ns() - t0);
    };

    std::vector<Unpadded> shared(nthreads);
    std::vector<Padded>   padded(nthreads);
    // Best of 5: a single pair of runs swung by 70% on a laptop.
    double per_shared = 1e30, per_padded = 1e30;
    for (int r = 0; r < 5; ++r) {
        per_shared = std::min(per_shared, run(shared.data()) / double(kIters));
        per_padded = std::min(per_padded, run(padded.data()) / double(kIters));
    }

    std::printf("  %-44s %8.3f ns\n", "counter++ sharing a cache line", per_shared);
    std::printf("  %-44s %8.3f ns   alignas(128)\n", "counter++ on its own line", per_padded);
    std::printf("  %-44s %8.2fx\n", "cost of false sharing",
                per_padded > 0.0 ? per_shared / per_padded : 0.0);
    std::printf("  %u threads, each touching only its own counter; the sole\n"
                "  difference is whether those counters share a line.\n", nthreads);
    std::printf("  Plain stores, not atomics. The effect is modest on cores with\n"
                "  deep store buffers and large lines (Apple Silicon is 128 B) and\n"
                "  larger where lines are 64 B. Measure it on the target.\n");
}

void bench_containers() {
    section("LOCK-FREE CONTAINERS");

    constexpr std::uint64_t kN = 5'000'000;
    {
        container::SpscRing<std::uint64_t> ring(1024);
        bench("SPSC ring push+pop (same thread)", kN, [&] {
            std::uint64_t v;
            for (std::uint64_t i = 0; i < kN; ++i) { ring.try_push(i); ring.try_pop(v); }
            core::do_not_optimize(v);
        }, "cached index: no coherence miss");
    }
    {
        container::SpscRing<world::ShardCommand> ring(4096);
        world::ShardCommand cmd{};
        bench("SPSC ring push 48B command", kN, [&] {
            world::ShardCommand out;
            for (std::uint64_t i = 0; i < kN; ++i) { ring.try_push(cmd); ring.try_pop(out); }
        }, "the actual io -> shard payload");
    }
    {
        container::SpscByteRing ring(1 << 16);
        alignas(8) std::byte payload[256];
        bench("byte ring write+peek+pop (256B)", 2'000'000, [&] {
            for (std::uint64_t i = 0; i < 2'000'000; ++i) {
                std::byte* dst = ring.begin_write(256);
                if (dst) { std::memcpy(dst, payload, 256); ring.commit_write(256); }
                if (!ring.peek().empty()) ring.pop();
            }
        }, "variable length, contiguous records");
    }
    {
        container::SlotMap<std::uint64_t> map(4096);
        std::vector<container::Handle> hs;
        hs.reserve(4096);
        for (int i = 0; i < 4096; ++i) hs.push_back(map.insert(std::uint64_t(i)));
        bench("slot map find (generation-checked)", 5'000'000, [&] {
            std::uint64_t acc = 0;
            for (std::uint64_t i = 0; i < 5'000'000; ++i) {
                core::compiler_barrier();
                acc += *map.find(hs[i & 4095]);
            }
            core::do_not_optimize(acc);
        }, "two loads + a generation compare");
    }

    // Cross-thread handoff latency: the number that matters for the io -> shard
    // path, measured as a round trip through two rings and halved.
    {
        container::SpscRing<std::uint64_t> to_b(1024), to_a(1024);
        std::atomic<bool> run{true};
        std::thread b([&] {
            std::uint64_t v;
            while (run.load(std::memory_order_relaxed)) {
                if (to_b.try_pop(v)) while (!to_a.try_push(v)) core::cpu_relax();
                else core::cpu_relax();
            }
        });
        constexpr std::uint64_t kR = 200'000;
        bench("SPSC cross-core round trip", kR, [&] {
            std::uint64_t v;
            for (std::uint64_t i = 0; i < kR; ++i) {
                while (!to_b.try_push(i)) core::cpu_relax();
                while (!to_a.try_pop(v)) core::cpu_relax();
            }
        }, "round trip; one-way is about half");
        run.store(false);
        b.join();
    }
}

// --------------------------------------------------------------- allocation
void bench_allocation() {
    section("ALLOCATION  (why the hot path uses neither malloc nor new)");

    constexpr std::uint64_t kN = 2'000'000;
    {
        alloc::Arena a(1 << 22);
        a.prefault();
        bench("arena bump allocate 64B", kN, [&] {
            for (std::uint64_t i = 0; i < kN; ++i) {
                if ((i & 0xFFFF) == 0) a.reset();
                core::do_not_optimize(a.allocate(64, 8));
            }
        }, "pointer add");
    }
    {
        struct Obj { std::uint64_t a, b, c, d; };
        alloc::Pool<Obj> pool(1024);
        bench("pool acquire+release", kN, [&] {
            for (std::uint64_t i = 0; i < kN; ++i) {
                Obj* o = pool.acquire();
                core::do_not_optimize(o);
                pool.release(o);
            }
        }, "intrusive free list");
    }
    {
        bench("operator new + delete 64B", 1'000'000, [&] {
            for (std::uint64_t i = 0; i < 1'000'000; ++i) {
                auto* p = new std::uint64_t[8];
                core::do_not_optimize(p);
                delete[] p;
            }
        }, "the thing being avoided");
    }
    {
        bench("malloc + free 64B", 1'000'000, [&] {
            for (std::uint64_t i = 0; i < 1'000'000; ++i) {
                void* p = std::malloc(64);
                core::do_not_optimize(p);
                std::free(p);
            }
        }, "");
    }
}

// ----------------------------------------------------------------- metrics
void bench_metrics() {
    section("MEASUREMENT INSTRUMENTS  (cost of observing the system)");

    constexpr std::uint64_t kN = 5'000'000;
    {
        metrics::Histogram h;
        core::Xoshiro256ss rng(1);
        std::vector<std::uint64_t> vals(4096);
        for (auto& v : vals) v = 500 + rng.next_below(2'000'000);
        bench("histogram record", kN, [&] {
            for (std::uint64_t i = 0; i < kN; ++i) h.record(vals[i & 4095]);
        }, "clz + two shifts + increment");
    }
    bench("Clock::now_ns", kN, [&] {
        std::int64_t acc = 0;
        for (std::uint64_t i = 0; i < kN; ++i) acc += Clock::now_ns();
        core::do_not_optimize(acc);
    }, "commpage/vDSO read, no syscall");

    {
        metrics::Histogram h;
        for (int i = 0; i < 100'000; ++i) h.record(std::uint64_t(i * 37 % 900'000));
        bench("histogram percentile(99.9)", 20'000, [&] {
            for (int i = 0; i < 20'000; ++i) core::do_not_optimize(h.percentile(99.9));
        }, "reporting path only, never hot");
    }
}

// ------------------------------------------------------------------- world
void bench_world() {
    section("SIMULATION KERNELS");

    constexpr int kN = 4096;
    core::Xoshiro256ss rng(7);
    std::vector<float> xs(kN), ys(kN);
    for (int i = 0; i < kN; ++i) {
        xs[std::size_t(i)] = float(rng.next_double()) * 1024.0f;
        ys[std::size_t(i)] = float(rng.next_double()) * 1024.0f;
    }

    // The kernel the shard actually runs. It is deliberately *not* the tight
    // vectorisable loop the old point simulation had: a snake steers (atan2,
    // sin, cos), can be dead, and drops body points at irregular intervals, so
    // this is a branchy scalar loop by nature. Measuring the real one matters
    // more than measuring a flattering one -- a benchmark for code that is not
    // in the product is worse than no benchmark.
    constexpr int kIters = 2000;
    std::vector<float> hd(kN), wh(kN), sd(kN);
    std::vector<std::uint8_t> alive(kN, 1);
    for (int i = 0; i < kN; ++i) {
        hd[std::size_t(i)] = float(rng.next_double() * 6.2831853);
        wh[std::size_t(i)] = float(rng.next_double() * 6.2831853);
    }
    bench("snake advance 4096 (per snake)", std::uint64_t(kIters) * kN, [&] {
        const float dt = 1.0f / 60.0f, max_turn = 4.5f / 60.0f;
        for (int it = 0; it < kIters; ++it) {
            core::compiler_barrier();
            for (int i = 0; i < kN; ++i) {
                const auto k = std::size_t(i);
                if (!alive[k]) continue;
                float d = wh[k] - hd[k];
                while (d >  3.14159265f) d -= 6.28318531f;
                while (d < -3.14159265f) d += 6.28318531f;
                hd[k] += std::clamp(d, -max_turn, max_turn);
                xs[k] += std::cos(hd[k]) * 26.0f * dt;
                ys[k] += std::sin(hd[k]) * 26.0f * dt;
                if (xs[k] < 0 || xs[k] > 1024 || ys[k] < 0 || ys[k] > 1024) {
                    xs[k] = 512; ys[k] = 512;
                }
                sd[k] += 26.0f * dt;
            }
        }
    }, "steer + move; trig-bound, not vectorisable");

    world::SpatialGrid grid;
    grid.configure({0, 0, 1024, 1024}, 48.0f, kN);
    bench("grid rebuild 4096 points (per point)", std::uint64_t(2000) * kN, [&] {
        for (int it = 0; it < 2000; ++it) grid.rebuild(xs.data(), ys.data(), kN);
    }, "counting sort over body points, three passes");

    grid.rebuild(xs.data(), ys.data(), kN);
    bench("AOI query r=64 (per query)", 500'000, [&] {
        std::uint64_t hits = 0;
        for (std::uint64_t q = 0; q < 500'000; ++q) {
            const std::size_t i = q & 4095;
            grid.query_aabb(xs[i], ys[i], 64.0f, [&](std::span<const std::uint32_t> cell) {
                hits += cell.size();
            });
        }
        core::do_not_optimize(hits);
    }, "contiguous cell rows");
}

} // namespace

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    const auto probe = Clock::probe();

    std::printf("\n\033[1mge_bench -- component microbenchmarks\033[0m\n");
    std::printf("  clock overhead %lld ns, granularity %lld ns; batches are sized so neither matters.\n",
                static_cast<long long>(probe.overhead_ns), static_cast<long long>(probe.granularity_ns));
    std::printf("  best-of-7 batches reported; allocations counted per batch and should read 0.\n");
#ifndef NDEBUG
    std::printf("\n  \033[33mWARNING: assertions are enabled; these numbers are not representative.\033[0m\n");
#endif

    auto want = [&](const char* s) { return !filter || std::strstr(s, filter); };
    if (want("protocol"))   bench_protocol();
    if (want("container"))  bench_containers();
    if (want("allocation")) bench_allocation();
    if (want("metrics"))    bench_metrics();
    if (want("world"))      bench_world();
    if (want("sharing"))    bench_false_sharing();

    std::printf("\n  %zu cases. A non-zero allocation column outside the malloc/new cases\n"
                "  means something on that path is still reaching the heap.\n\n", g_results.size());
    return 0;
}
