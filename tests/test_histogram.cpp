#include "test.hpp"
#include <cmath>

#include <algorithm>
#include <vector>

#include "ge/core/bits.hpp"
#include "ge/metrics/histogram.hpp"

using namespace ge;
using metrics::Histogram;

namespace {
// Ground truth: the exact quantile from a sorted sample, using the same
// nearest-rank definition the histogram uses.
std::uint64_t exact_percentile(std::vector<std::uint64_t> v, double p) {
    std::sort(v.begin(), v.end());
    auto rank = std::size_t(std::ceil(p / 100.0 * double(v.size())));
    if (rank == 0) rank = 1;
    if (rank > v.size()) rank = v.size();
    return v[rank - 1];
}
} // namespace

GE_TEST(histogram_basic_counts) {
    Histogram h;
    for (int i = 1; i <= 100; ++i) h.record(std::uint64_t(i));
    CHECK_EQ(h.count(), 100u);
    CHECK_EQ(h.min(), 1u);
    CHECK_EQ(h.max(), 100u);
    CHECK_NEAR(h.mean(), 50.5, 0.6);
}

// The contract of an HDR histogram is bounded *relative* error, and 3
// significant figures means every percentile must land within 0.1% of truth.
GE_TEST(histogram_percentiles_within_relative_error) {
    core::Xoshiro256ss rng(12345);
    std::vector<std::uint64_t> samples;
    samples.reserve(200'000);
    Histogram h;

    // A realistic latency shape: a tight body with a heavy tail, spanning four
    // orders of magnitude. A uniform distribution would not exercise the
    // bucketing at all.
    for (int i = 0; i < 200'000; ++i) {
        const double u = rng.next_double();
        std::uint64_t v;
        if (u < 0.99)      v = 1'000 + std::uint64_t(rng.next_below(9'000));
        else if (u < 0.999) v = 50'000 + std::uint64_t(rng.next_below(200'000));
        else                v = 1'000'000 + std::uint64_t(rng.next_below(9'000'000));
        samples.push_back(v);
        h.record(v);
    }

    for (double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
        const double want = double(exact_percentile(samples, p));
        const double got  = double(h.percentile(p));
        const double rel  = std::abs(got - want) / want;
        if (rel > 0.001) {
            char buf[192];
            std::snprintf(buf, sizeof buf, "p%.2f: histogram %.0f vs exact %.0f (%.4f%% error)",
                          p, got, want, rel * 100);
            ge::test::fail(__FILE__, __LINE__, buf);
        }
        ++ge::test::g_checks;
    }
}

GE_TEST(histogram_merge_is_equivalent_to_recording_together) {
    Histogram a, b, both;
    core::Xoshiro256ss rng(7);
    for (int i = 0; i < 50'000; ++i) {
        const std::uint64_t v = 100 + rng.next_below(100'000);
        if (i % 2) { a.record(v); } else { b.record(v); }
        both.record(v);
    }
    a.merge(b);
    CHECK_EQ(a.count(), both.count());
    CHECK_EQ(a.min(), both.min());
    CHECK_EQ(a.max(), both.max());
    CHECK_EQ(a.percentile(50), both.percentile(50));
    CHECK_EQ(a.percentile(99.9), both.percentile(99.9));
}

// Without correction, one long stall looks like a single slow sample. With it,
// the requests the stall prevented from being issued are accounted for -- which
// is the difference between a p99 of 1ms and a p99 of 50ms on the same run.
GE_TEST(histogram_coordinated_omission_correction) {
    Histogram naive, corrected;
    constexpr std::uint64_t kInterval = 1'000'000;    // one request per ms

    for (int i = 0; i < 9'999; ++i) {
        naive.record(1'000);
        corrected.record_corrected(1'000, kInterval);
    }
    // One 1-second stall. At one request per ms, a real client would have had
    // ~1000 requests queued behind it, every one of them slow.
    naive.record(1'000'000'000);
    corrected.record_corrected(1'000'000'000, kInterval);

    CHECK_EQ(naive.count(), 10'000u);
    CHECK_EQ(corrected.count(), 10'999u);             // 999 back-filled samples

    // Uncorrected, a full second of unavailability is one sample in ten
    // thousand: it is invisible at every percentile the report quotes and shows
    // up only as max. This is exactly how a stalled server passes a latency SLA.
    CHECK(naive.percentile(99) < 2'000u);
    CHECK(naive.percentile(99.9) < 2'000u);
    CHECK(naive.percentile(99.99) < 2'000u);
    CHECK(naive.max() >= 1'000'000'000u);

    // Corrected, 9% of the run is slow and the tail says so.
    CHECK(corrected.percentile(99) > 100'000'000u);
    CHECK(corrected.percentile(95) > 1'000u);
}

GE_TEST(histogram_clamps_rather_than_overflowing) {
    Histogram h(1'000'000, 3);
    h.record(5'000'000);                              // above the ceiling
    CHECK_EQ(h.count(), 1u);
    CHECK(h.max() >= 1'000'000u);
}

GE_TEST(histogram_empty_is_well_defined) {
    Histogram h;
    CHECK_EQ(h.count(), 0u);
    CHECK_EQ(h.percentile(99), 0u);
    CHECK_EQ(h.min(), 0u);
    CHECK_EQ(h.mean(), 0.0);
    CHECK_EQ(h.stddev(), 0.0);
}

GE_TEST(histogram_memory_is_bounded_regardless_of_sample_count) {
    Histogram h;
    const std::size_t before = h.memory_bytes();
    core::Xoshiro256ss rng(1);
    for (int i = 0; i < 1'000'000; ++i) h.record(1 + rng.next_below(50'000'000));
    CHECK_EQ(h.memory_bytes(), before);               // fixed footprint, 1M samples
    CHECK(before < 512 * 1024);
}
