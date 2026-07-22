#include "test.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "ge/core/bits.hpp"
#include "ge/lb/balancer.hpp"

using namespace ge;

namespace {
std::vector<lb::BackendConfig> make_backends(int n) {
    std::vector<lb::BackendConfig> v;
    for (int i = 0; i < n; ++i)
        v.push_back({"10.0.0." + std::to_string(i + 1), std::uint16_t(9000 + i), 1});
    return v;
}
} // namespace

namespace {
// Coefficient of variation: the standard deviation of the per-backend shares
// divided by their mean. This is the quantity consistent hashing actually
// bounds, and the one worth asserting on.
double coefficient_of_variation(const std::vector<double>& v) {
    double mean = 0;
    for (double x : v) mean += x;
    mean /= double(v.size());
    double var = 0;
    for (double x : v) var += (x - mean) * (x - mean);
    return std::sqrt(var / double(v.size())) / mean;
}
} // namespace

// Virtual nodes exist to make the split even. The theory says the coefficient
// of variation of the key share is ~1/sqrt(V); at the configured V=512 that is
// about 4.4%. Asserting on that -- rather than on a per-backend percentage --
// is what makes this test stable instead of a coin flip, and it fails loudly if
// the hash ever loses its avalanche (unfinalised FNV-1a scores ~25% here).
GE_TEST(consistent_hash_distributes_evenly) {
    lb::BalancerConfig cfg;
    lb::Balancer b;
    b.configure(make_backends(4), cfg);

    std::vector<double> hits(4, 0);
    constexpr int kKeys = 200'000;
    for (int i = 1; i <= kKeys; ++i) {
        const int idx = b.pick(std::uint32_t(i), 0);
        CHECK(idx >= 0 && idx < 4);
        hits[std::size_t(idx)] += 1.0;
    }
    const double predicted = 1.0 / std::sqrt(double(cfg.virtual_nodes));
    const double cv = coefficient_of_variation(hits);
    if (cv > predicted * 2.5) {
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "key-share CV %.4f exceeds 2.5x the predicted %.4f for V=%u",
                      cv, predicted, cfg.virtual_nodes);
        ge::test::fail(__FILE__, __LINE__, buf);
    }
    ++ge::test::g_checks;

    double total = 0;
    for (double h : hits) total += h;
    CHECK_EQ(total, double(kKeys));       // every key was routed exactly once
}

GE_TEST(consistent_hash_is_stable_across_calls) {
    lb::Balancer b;
    b.configure(make_backends(5), lb::BalancerConfig{});
    for (std::uint32_t s = 1; s < 2000; ++s)
        CHECK_EQ(b.pick(s, 0), b.pick(s, 0));    // affinity means deterministic
}

// The whole reason for a hash ring instead of modulo-N: losing a backend must
// move only the keys that lived on it.
GE_TEST(consistent_hash_remaps_only_the_lost_share) {
    lb::BalancerConfig cfg;
    cfg.eject_after_failures = 1;

    lb::Balancer b;
    b.configure(make_backends(4), cfg);

    std::vector<int> before(20'000);
    for (std::uint32_t s = 0; s < 20'000; ++s) before[s] = b.pick(s + 1, 0);

    // Take backend 2 out of rotation.
    int victim = 2;
    b.on_probe_timeout(victim, 1000);
    CHECK_EQ(b.healthy_count(), 3u);

    int moved = 0, owned_by_victim = 0;
    for (std::uint32_t s = 0; s < 20'000; ++s) {
        const int now = b.pick(s + 1, 1000);
        CHECK(now != victim);
        if (before[s] == victim) ++owned_by_victim;
        if (before[s] != now)    ++moved;
    }
    // Every key that moved is a key the victim owned; nothing else was disturbed.
    CHECK_EQ(moved, owned_by_victim);
    CHECK(owned_by_victim > 20'000 / 8);          // it did own a real share
    CHECK(owned_by_victim < 20'000 / 2);
}

GE_TEST(key_space_share_is_balanced) {
    lb::BalancerConfig cfg;
    lb::Balancer b;
    b.configure(make_backends(8), cfg);

    const auto share = b.key_space_share();
    double total = 0;
    for (double s : share) total += s;
    CHECK_NEAR(total, 1.0, 1e-6);         // the ring covers the whole key space

    const double predicted = 1.0 / std::sqrt(double(cfg.virtual_nodes));
    CHECK(coefficient_of_variation(share) < predicted * 2.5);
}

GE_TEST(weighted_backends_get_proportional_share) {
    std::vector<lb::BackendConfig> bs = make_backends(3);
    bs[0].weight = 3;                              // three times the capacity
    lb::Balancer b;
    b.configure(bs, lb::BalancerConfig{});
    const auto share = b.key_space_share();
    CHECK_NEAR(share[0] / share[1], 3.0, 0.35);
}

// A single failed probe must not take a backend out: that turns one dropped
// packet into a capacity event.
GE_TEST(ejection_requires_a_failure_streak) {
    lb::BalancerConfig cfg;
    cfg.eject_after_failures = 3;
    cfg.eject_cooldown_ns    = 1'000;

    lb::Balancer b;
    b.configure(make_backends(2), cfg);
    b.on_probe_timeout(0, 0);
    CHECK_EQ(b.healthy_count(), 2u);
    b.on_probe_timeout(0, 0);
    CHECK_EQ(b.healthy_count(), 2u);
    b.on_probe_timeout(0, 0);
    CHECK_EQ(b.healthy_count(), 1u);              // third strike

    // A good reply resets the streak.
    b.tick(2'000);
    b.on_probe_reply(0, 500'000, 0);
    CHECK_EQ(b.healthy_count(), 2u);
    b.on_probe_timeout(0, 3'000);
    CHECK_EQ(b.healthy_count(), 2u);
}

GE_TEST(ejected_backend_is_readmitted_after_cooldown) {
    lb::BalancerConfig cfg;
    cfg.eject_after_failures = 1;
    cfg.eject_cooldown_ns    = 5'000;

    lb::Balancer b;
    b.configure(make_backends(2), cfg);
    b.on_probe_timeout(0, 1'000);
    CHECK_EQ(b.healthy_count(), 1u);
    CHECK(b.pick(0, 1'000) != 0);

    b.tick(3'000);                                 // still cooling down
    CHECK_EQ(b.healthy_count(), 1u);
    b.tick(7'000);                                 // cooldown expired
    CHECK_EQ(b.healthy_count(), 2u);

    // Half-open: it is readmitted with the streak primed, so one more failure
    // ejects it again immediately instead of costing another full streak.
    b.on_probe_timeout(0, 8'000);
    CHECK_EQ(b.healthy_count(), 1u);
}

GE_TEST(pick_returns_negative_when_everything_is_ejected) {
    lb::BalancerConfig cfg;
    cfg.eject_after_failures = 1;
    lb::Balancer b;
    b.configure(make_backends(2), cfg);
    b.on_probe_timeout(0, 0);
    b.on_probe_timeout(1, 0);
    CHECK_EQ(b.pick(1234, 0), -1);
    CHECK_EQ(b.pick(0, 0), -1);
}

// Without affinity, selection is load-aware: the backend carrying fewer
// sessions should win.
GE_TEST(power_of_two_choices_prefers_the_lighter_backend) {
    lb::BalancerConfig cfg;
    cfg.session_affinity = false;
    lb::Balancer b;
    b.configure(make_backends(2), cfg);

    for (int i = 0; i < 100; ++i) b.on_session_opened(0);   // backend 0 is loaded
    b.on_probe_reply(0, 2'000'000, 80);
    b.on_probe_reply(1, 200'000, 5);

    int to_one = 0;
    for (int i = 0; i < 200; ++i)
        if (b.pick(0, 0) == 1) ++to_one;
    CHECK(to_one > 150);
}
