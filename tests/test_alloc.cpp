#include "test.hpp"

#include <string>
#include <vector>

#include "ge/alloc/arena.hpp"
#include "ge/alloc/counters.hpp"
#include "ge/alloc/pool.hpp"

using namespace ge;

GE_TEST(arena_bumps_and_aligns) {
    alloc::Arena a(4096);
    void* p1 = a.allocate(10, 8);
    void* p2 = a.allocate(1, 64);
    void* p3 = a.allocate(100, 16);
    CHECK(p1 && p2 && p3);
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(p2) % 64, 0u);
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(p3) % 16, 0u);
    CHECK(a.used() >= 111u);
}

GE_TEST(arena_returns_null_when_exhausted_not_abort) {
    alloc::Arena a(4096);
    CHECK(a.allocate(4096, 1) != nullptr);
    CHECK(a.allocate(1, 1) == nullptr);   // caller treats this as backpressure
}

GE_TEST(arena_reset_reuses_the_same_memory) {
    alloc::Arena a(4096);
    void* first = a.allocate(64, 8);
    const std::size_t used = a.used();
    a.reset();
    CHECK_EQ(a.used(), 0u);
    CHECK_EQ(a.allocate(64, 8), first);   // the per-tick pattern: no free, no reallocation
    CHECK(a.high_water() >= used);
}

GE_TEST(arena_scope_releases_to_mark) {
    alloc::Arena a(4096);
    a.allocate(32, 8);
    const std::size_t base = a.used();
    {
        alloc::ArenaScope s(a);
        a.allocate(512, 8);
        CHECK(a.used() > base);
    }
    CHECK_EQ(a.used(), base);
}

GE_TEST(pool_recycles_slots_and_bounds_capacity) {
    struct Obj { std::uint64_t a, b; };
    alloc::Pool<Obj> pool(4);
    Obj* objs[4];
    for (int i = 0; i < 4; ++i) {
        objs[i] = pool.acquire();
        CHECK(objs[i] != nullptr);
    }
    CHECK(pool.acquire() == nullptr);     // bounded by construction
    CHECK_EQ(pool.live(), 4u);

    pool.release(objs[2]);
    CHECK_EQ(pool.live(), 3u);
    Obj* again = pool.acquire();
    CHECK_EQ(again, objs[2]);             // slot reuse, no allocation
    CHECK_EQ(pool.high_water(), 4u);
}

GE_TEST(pool_index_round_trips) {
    struct Obj { std::uint64_t a, b; };
    alloc::Pool<Obj> pool(16);
    for (std::uint32_t i = 0; i < 16; ++i) {
        Obj* o = pool.acquire();
        CHECK_EQ(pool.index_of(o), i);
        CHECK_EQ(pool.at(i), o);
    }
}

// The probe is the evidence behind every "zero allocations" claim in the
// report, so it gets tested like any other measurement instrument.
GE_TEST(alloc_probe_counts_new_and_delete) {
    if (!alloc::probe_enabled()) return;
    alloc::AllocScope scope;
    auto* p = new std::uint64_t[128];
    p[0] = 1;
    core::do_not_optimize(p);
    const auto after_new = scope.delta();
    CHECK_EQ(after_new.allocs, 1u);
    CHECK(after_new.bytes_allocated >= 128 * sizeof(std::uint64_t));

    delete[] p;
    const auto after_delete = scope.delta();
    CHECK_EQ(after_delete.allocs, 1u);
    CHECK_EQ(after_delete.frees, 1u);
    CHECK_EQ(after_delete.live_allocs(), 0);
}

GE_TEST(alloc_probe_sees_container_growth) {
    if (!alloc::probe_enabled()) return;
    alloc::AllocScope scope;
    std::vector<int> v;
    for (int i = 0; i < 1000; ++i) v.push_back(i);
    core::do_not_optimize(v.data());
    CHECK(scope.delta().allocs > 1);     // growth reallocates; that is the point
}

// The arena and pool exist so that hot loops allocate nothing. This asserts it
// rather than trusting it.
GE_TEST(arena_and_pool_hot_loops_allocate_nothing) {
    if (!alloc::probe_enabled()) return;
    struct Obj { std::uint64_t a, b; };
    alloc::Arena arena(1 << 16);
    alloc::Pool<Obj> pool(64);
    arena.prefault();

    alloc::AllocScope scope;               // start measuring after construction
    for (int tick = 0; tick < 10'000; ++tick) {
        arena.reset();
        for (int i = 0; i < 8; ++i) core::do_not_optimize(arena.allocate(64, 8));
        Obj* o = pool.acquire();
        core::do_not_optimize(o);
        pool.release(o);
    }
    const auto d = scope.delta();
    CHECK_EQ(d.allocs, 0u);
    CHECK_EQ(d.frees, 0u);
}
