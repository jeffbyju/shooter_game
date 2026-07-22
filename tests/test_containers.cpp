#include "test.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "ge/container/mpmc_queue.hpp"
#include "ge/container/slot_map.hpp"
#include "ge/container/spsc_byte_ring.hpp"
#include "ge/container/spsc_ring.hpp"

using namespace ge;

GE_TEST(spsc_ring_fifo_and_capacity) {
    container::SpscRing<int> r(8);
    CHECK_EQ(r.capacity(), 8u);
    for (int i = 0; i < 8; ++i) CHECK(r.try_push(i));
    CHECK(!r.try_push(99));                       // bounded: refuses rather than grows
    int v = -1;
    for (int i = 0; i < 8; ++i) { CHECK(r.try_pop(v)); CHECK_EQ(v, i); }
    CHECK(!r.try_pop(v));
}

GE_TEST(spsc_ring_drain_batches) {
    container::SpscRing<int> r(16);
    for (int i = 0; i < 10; ++i) r.try_push(i);
    int sum = 0;
    const std::size_t n = r.drain([&](int x) { sum += x; });
    CHECK_EQ(n, 10u);
    CHECK_EQ(sum, 45);
    CHECK(r.empty());
}

// The property that actually matters: with one producer and one consumer, every
// value crosses exactly once, in order, with no duplication or loss. Run long
// enough to exercise the wrap and the cached-index fast path.
GE_TEST(spsc_ring_concurrent_stress) {
    constexpr int kN = 200'000;
    container::SpscRing<std::uint64_t> r(1024);
    std::atomic<bool> done{false};
    std::uint64_t received = 0;
    std::uint64_t expected_next = 0;
    bool ordered = true;

    std::thread consumer([&] {
        std::uint64_t v;
        while (received < kN) {
            if (r.try_pop(v)) {
                if (v != expected_next) ordered = false;
                ++expected_next;
                ++received;
            } else if (done.load(std::memory_order_acquire) && r.empty()) {
                break;
            }
        }
    });

    for (std::uint64_t i = 0; i < kN; ++i)
        while (!r.try_push(i)) core::cpu_relax();
    done.store(true, std::memory_order_release);
    consumer.join();

    CHECK_EQ(received, std::uint64_t(kN));
    CHECK(ordered);
}

GE_TEST(byte_ring_records_are_contiguous_across_wrap) {
    // Small ring and odd record sizes force many wraps; the invariant under test
    // is that peek() always returns one contiguous span, which is what makes
    // the zero-copy handoff to write(2) legal.
    container::SpscByteRing ring(256);
    std::vector<std::byte> payload(37);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = std::byte(i);

    int written = 0, read = 0;
    for (int round = 0; round < 500; ++round) {
        if (ring.try_write(payload.data(), std::uint32_t(payload.size()))) ++written;
        auto rec = ring.peek();
        if (!rec.empty()) {
            CHECK_EQ(rec.size(), payload.size());
            bool same = true;
            for (std::size_t i = 0; i < rec.size(); ++i)
                if (rec[i] != payload[i]) same = false;
            CHECK(same);
            ring.pop();
            ++read;
        }
    }
    CHECK_EQ(written, read);
    CHECK(written > 400);
}

GE_TEST(byte_ring_in_place_write) {
    container::SpscByteRing ring(1024);
    std::byte* dst = ring.begin_write(64);
    CHECK(dst != nullptr);
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(dst) % 8, 0u);   // alignment invariant
    for (int i = 0; i < 16; ++i) dst[i] = std::byte(i + 1);
    ring.commit_write(16);

    auto rec = ring.peek();
    CHECK_EQ(rec.size(), 16u);
    CHECK_EQ(int(rec[0]), 1);
    CHECK_EQ(int(rec[15]), 16);
    ring.pop();
    CHECK(ring.empty());
}

GE_TEST(byte_ring_refuses_when_full) {
    container::SpscByteRing ring(128);
    int ok = 0;
    while (ring.begin_write(32)) { ring.commit_write(32); ++ok; }
    CHECK(ok > 0);
    CHECK(ring.begin_write(32) == nullptr);      // backpressure, not growth
}

GE_TEST(mpmc_queue_multi_producer) {
    constexpr int kProducers = 4, kPer = 20'000;
    container::MpmcQueue<std::uint64_t> q(4096);
    std::atomic<int> produced{0};
    std::vector<std::thread> ts;
    for (int p = 0; p < kProducers; ++p)
        ts.emplace_back([&, p] {
            for (int i = 0; i < kPer; ++i) {
                const std::uint64_t v = std::uint64_t(p) * kPer + std::uint64_t(i);
                while (!q.try_push(v)) core::cpu_relax();
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });

    std::vector<bool> seen(std::size_t(kProducers) * kPer, false);
    int got = 0;
    std::uint64_t v;
    while (got < kProducers * kPer) {
        if (q.try_pop(v)) {
            CHECK(!seen[v]);                      // no duplicates
            seen[v] = true;
            ++got;
        }
    }
    for (auto& t : ts) t.join();
    CHECK_EQ(got, kProducers * kPer);
}

// The reason for generations: a handle held across an erase must not silently
// resolve to whatever entity took over the slot.
GE_TEST(slot_map_generation_prevents_stale_access) {
    container::SlotMap<int> m(16);
    const auto a = m.insert(10);
    const auto b = m.insert(20);
    CHECK_EQ(*m.find(a), 10);
    CHECK_EQ(*m.find(b), 20);

    CHECK(m.erase(a));
    CHECK(m.find(a) == nullptr);                  // stale handle resolves to null
    const auto c = m.insert(30);                  // reuses a's slot
    CHECK_EQ(c.index, a.index);
    CHECK(c.gen != a.gen);
    CHECK(m.find(a) == nullptr);                  // ...and the old handle still does not alias it
    CHECK_EQ(*m.find(c), 30);
    CHECK_EQ(m.size(), 2u);
}

GE_TEST(slot_map_stays_dense_after_erase) {
    container::SlotMap<int> m(64);
    std::vector<container::Handle> hs;
    for (int i = 0; i < 32; ++i) hs.push_back(m.insert(i));
    for (int i = 0; i < 32; i += 2) CHECK(m.erase(hs[std::size_t(i)]));
    CHECK_EQ(m.size(), 16u);

    int sum = 0;
    for (int v : m) sum += v;                     // iteration is over packed values
    CHECK_EQ(sum, 1 + 3 + 5 + 7 + 9 + 11 + 13 + 15 + 17 + 19 + 21 + 23 + 25 + 27 + 29 + 31);
    for (int i = 1; i < 32; i += 2) CHECK_EQ(*m.find(hs[std::size_t(i)]), i);
}

GE_TEST(handle_survives_wire_roundtrip) {
    const container::Handle h{12345, 678};
    const auto back = container::Handle::from_bits(h.bits());
    CHECK(back == h);
}
