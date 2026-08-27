#pragma once
// HDR histogram: constant-time recording, bounded memory, and a guaranteed
// relative error across the whole dynamic range (1ns .. 60s at 3 significant
// figures here). Percentiles are computed from the stored distribution, not
// from a reservoir sample, so p99.99 is exact to within the bucket width rather
// than being a guess from a subsample.
//
// Why not sort a vector of samples? Because a 10M-sample run would need 80MB
// and a sort on the reporting path, and because the recording call would touch
// a growing buffer -- both of which perturb the latencies being measured. This
// records in ~4ns into a fixed 220KB working set.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "ge/core/assert.hpp"
#include "ge/core/platform.hpp"
#include "ge/metrics/counters.hpp"

namespace ge::metrics {

class Histogram {
public:
    // 60s ceiling: anything slower than that is a hang, not a latency.
    explicit Histogram(std::uint64_t highest = 60'000'000'000ULL, int significant_digits = 3) {
        GE_CHECK(significant_digits >= 1 && significant_digits <= 5);
        std::uint64_t largest = 2;
        for (int i = 0; i < significant_digits; ++i) largest *= 10;
        sub_bucket_count_mag_ = int(64 - std::countl_zero(largest - 1));
        sub_bucket_count_     = std::uint64_t{1} << sub_bucket_count_mag_;
        sub_bucket_half_      = sub_bucket_count_ / 2;
        sub_bucket_half_mag_  = sub_bucket_count_mag_ - 1;
        sub_bucket_mask_      = sub_bucket_count_ - 1;

        bucket_count_ = 1;
        std::uint64_t smallest_untrackable = sub_bucket_count_;
        while (smallest_untrackable <= highest && smallest_untrackable < (std::uint64_t{1} << 63)) {
            smallest_untrackable <<= 1;
            ++bucket_count_;
        }
        counts_.assign(std::size_t(std::uint64_t(bucket_count_ + 1) * sub_bucket_half_), 0);
        highest_ = highest;
    }

    // ~4ns: a leading-zero count, two shifts, one increment. No branches on the
    // common path beyond the range clamp.
    GE_ALWAYS_INLINE void record(std::uint64_t value) noexcept {
        counts_[index_for(value)] += 1;
        ++count_;
        sum_ += value;
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
    }

    // Coordinated-omission correction.
    //
    // If a sample took longer than the interval at which samples were supposed
    // to be issued, every request that *would* have been issued during the stall
    // is missing from the data -- and those are exactly the slow ones. Without
    // this, a server that freezes for 100ms records one 100ms sample instead of
    // the ~1000 delayed samples a real client would have experienced, and the
    // tail looks far better than it is. This back-fills the implied samples.
    GE_ALWAYS_INLINE void record_corrected(std::uint64_t value, std::uint64_t expected_interval) noexcept {
        record(value);
        if (expected_interval == 0 || value <= expected_interval) return;
        for (std::uint64_t missing = value - expected_interval;
             missing >= expected_interval; missing -= expected_interval) {
            counts_[index_for(missing)] += 1;
            ++count_;
            sum_ += missing;
        }
    }

    std::uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        const double target_d = (p / 100.0) * double(count_);
        std::uint64_t target = std::uint64_t(std::ceil(target_d));
        if (target == 0) target = 1;
        if (target > count_) target = count_;

        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= target) return highest_equivalent(value_at(i));
        }
        return max_;
    }

    std::uint64_t min()   const noexcept { return count_.get() ? min_.get() : 0u; }
    std::uint64_t max()   const noexcept { return max_; }
    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t total() const noexcept { return sum_; }
    double        mean()  const noexcept { return count_ ? double(sum_) / double(count_) : 0.0; }

    double stddev() const noexcept {
        if (count_ < 2) return 0.0;
        const double m = mean();
        double acc = 0.0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            if (!counts_[i]) continue;
            const double d = double(median_equivalent(value_at(i))) - m;
            acc += d * d * double(counts_[i]);
        }
        return std::sqrt(acc / double(count_ - 1));
    }

    // Aggregating per-thread histograms at report time keeps recording
    // contention-free: each thread owns its own, nothing is shared until the end.
    void merge(const Histogram& o) {
        GE_CHECK(counts_.size() == o.counts_.size());
        for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
        count_ += o.count_;
        sum_   += o.sum_;
        min_ = std::min(min_, o.min_);
        max_ = std::max(max_, o.max_);
    }

    void reset() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0);
        count_ = sum_ = max_ = 0;
        min_ = UINT64_MAX;
    }

    std::size_t memory_bytes() const noexcept { return counts_.capacity() * sizeof(std::uint64_t); }

    // Walks every non-empty bucket in ascending order, handing the callback the
    // bucket's value range, its count, and the running cumulative total.
    //
    // Seven percentiles describe a distribution about as well as seven points
    // describe a curve. Exporting the buckets lets a reader plot the actual CDF
    // and see whether the tail is one stall, a bimodal split between two code
    // paths, or a genuine heavy tail -- three very different problems that can
    // produce the same p99.
    template <typename Fn>
    void for_each_bucket(Fn&& fn) const {
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            if (!counts_[i]) continue;
            const std::uint64_t lo = value_at(i);
            cumulative += counts_[i];
            fn(lo, highest_equivalent(lo), counts_[i], cumulative);
        }
    }

private:
    GE_ALWAYS_INLINE std::size_t index_for(std::uint64_t value) const noexcept {
        if (GE_UNLIKELY(value > highest_)) value = highest_;
        const int bucket = int(64 - std::countl_zero(value | sub_bucket_mask_)) - sub_bucket_count_mag_;
        const std::uint64_t sub = value >> bucket;
        return std::size_t((std::uint64_t(bucket + 1) << sub_bucket_half_mag_) + (sub - sub_bucket_half_));
    }

    std::uint64_t value_at(std::size_t index) const noexcept {
        std::int64_t bucket = std::int64_t(index >> sub_bucket_half_mag_) - 1;
        std::uint64_t sub   = (index & (sub_bucket_half_ - 1)) + sub_bucket_half_;
        if (bucket < 0) { sub -= sub_bucket_half_; bucket = 0; }
        return sub << bucket;
    }

    std::uint64_t bucket_width(std::uint64_t value) const noexcept {
        const int bucket = std::max(0, int(64 - std::countl_zero(value | sub_bucket_mask_)) - sub_bucket_count_mag_);
        return std::uint64_t{1} << bucket;
    }
    std::uint64_t highest_equivalent(std::uint64_t v) const noexcept { return v + bucket_width(v) - 1; }
    std::uint64_t median_equivalent(std::uint64_t v)  const noexcept { return v + bucket_width(v) / 2; }

    // Counter, not plain uint64: a histogram is recorded on the owning thread
    // and merged from the reporting thread, which is the same cross-thread read
    // the counters have -- and ThreadSanitizer flagged every bucket. Relaxed
    // atomics on both sides, which on arm64 and x86-64 is the same instruction
    // as a plain store; see counters.hpp for the assembly. The shape of the
    // hot path is unchanged: one load, one add, one store per record().
    std::vector<Counter> counts_;
    Counter       sum_   = 0;
    Counter       count_ = 0;
    Counter       min_   = UINT64_MAX;
    Counter       max_   = 0;
    std::uint64_t highest_ = 0;
    std::uint64_t sub_bucket_count_ = 0;
    std::uint64_t sub_bucket_half_  = 0;
    std::uint64_t sub_bucket_mask_  = 0;
    int           sub_bucket_count_mag_ = 0;
    int           sub_bucket_half_mag_  = 0;
    int           bucket_count_         = 0;
};

// The percentile set quoted throughout docs/. p99.9 and p99.99 are included
// because a server at 1M msg/s produces 1000 samples/s beyond p99.9 -- that is
// a tail users hit continuously, not a rounding artefact.
struct LatencySummary {
    std::uint64_t count = 0;
    std::uint64_t min = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    double        mean = 0, stddev = 0;

    static LatencySummary of(const Histogram& h) {
        return {h.count(), h.min(), h.percentile(50), h.percentile(90), h.percentile(95),
                h.percentile(99), h.percentile(99.9), h.percentile(99.99), h.max(),
                h.mean(), h.stddev()};
    }
};

} // namespace ge::metrics
