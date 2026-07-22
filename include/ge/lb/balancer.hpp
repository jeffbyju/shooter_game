#pragma once
// Backend selection for the gateway.
//
// Two mechanisms, used for different things:
//
// The load spread of a hash ring is governed by the virtual node count: the
// coefficient of variation of the per-backend key share is approximately
// 1/sqrt(V). At V=160 that is ~8% -- visible imbalance. At V=512 it is ~4.4%,
// which costs 512*N ring entries (a few tens of KB) and one extra step in a
// binary search. That trade is why the default is 512 and not the 100-200 most
// references quote.
//
// * Consistent hashing with virtual nodes decides *which backend a session
//   belongs to*. A session must keep landing on the same world server, because
//   its entity state lives there. Modulo-N hashing would remap every session
//   when one backend leaves; a ring with virtual nodes remaps only the ~1/N
//   that belonged to the departing node, and the virtual nodes keep the split
//   even (a 16-node ring without them has ~40% load spread).
//
// * Power-of-two-choices with a latency-weighted score decides *which of two
//   acceptable backends gets a new session*. Sampling two and taking the better
//   gets nearly the load-balance quality of querying all N at O(1) cost, and it
//   avoids the herd behaviour of "always pick the least loaded", where every
//   gateway simultaneously stampedes the one idle backend.
//
// Health is tracked by outlier ejection rather than a binary up/down flag: a
// backend that fails consecutive probes is removed from the ring for a cooldown
// and then readmitted, which keeps a briefly-slow node from being permanently
// blackholed and a flapping node from being re-added every probe interval.
#include <cstdint>
#include <string>
#include <vector>

#include "ge/core/platform.hpp"

namespace ge::lb {

struct BackendConfig {
    std::string   host;
    std::uint16_t port   = 0;
    std::uint32_t weight = 1;
};

struct BalancerConfig {
    std::uint32_t virtual_nodes       = 512;   // per backend; see the note above
    std::uint32_t eject_after_failures= 3;
    std::int64_t  eject_cooldown_ns   = 5'000'000'000;
    std::int64_t  probe_interval_ns   = 1'000'000'000;
    double        rtt_ewma_alpha      = 0.2;   // ~5-probe memory
    bool          session_affinity    = true;
};

struct BackendStats {
    std::uint64_t sessions_assigned = 0;
    std::uint64_t inflight          = 0;
    std::uint64_t probes_sent       = 0;
    std::uint64_t probes_ok         = 0;
    std::uint64_t probes_failed     = 0;
    std::uint64_t ejections         = 0;
    std::int64_t  rtt_ewma_ns       = 0;
    std::int64_t  last_rtt_ns       = 0;
    std::uint16_t reported_load_pct = 0;
    bool          healthy           = true;
};

class Balancer {
public:
    void configure(std::vector<BackendConfig> backends, const BalancerConfig& cfg);

    // Returns a backend index, or -1 when every backend is ejected. `session`
    // is the affinity key; pass 0 for "no affinity, balance freely".
    int pick(std::uint32_t session, std::int64_t now_ns) const noexcept;

    void on_probe_sent(int idx) noexcept;
    void on_probe_reply(int idx, std::int64_t rtt_ns, std::uint16_t load_pct) noexcept;
    void on_probe_timeout(int idx, std::int64_t now_ns) noexcept;
    void on_connect_failed(int idx, std::int64_t now_ns) noexcept;

    void on_session_opened(int idx) noexcept;
    void on_session_closed(int idx) noexcept;

    // Readmits backends whose cooldown has expired.
    void tick(std::int64_t now_ns) noexcept;

    std::size_t          size()  const noexcept { return backends_.size(); }
    const BackendConfig& config(int i) const noexcept { return backends_[std::size_t(i)]; }
    const BackendStats&  stats(int i)  const noexcept { return stats_[std::size_t(i)]; }
    std::size_t healthy_count() const noexcept;

    // Exposed for the distribution test: the fraction of the key space each
    // backend owns, which is what "even" actually means for a hash ring.
    std::vector<double> key_space_share() const;

private:
    struct RingNode {
        std::uint64_t hash;
        std::uint32_t backend;
    };

    int  ring_lookup(std::uint32_t session) const noexcept;
    // Lower is better: queue depth first, observed latency as the tiebreak.
    double score(int idx) const noexcept;
    bool   available(int idx, std::int64_t now_ns) const noexcept;

    std::vector<BackendConfig> backends_;
    std::vector<BackendStats>  stats_;
    std::vector<RingNode>      ring_;              // sorted by hash
    std::vector<std::uint32_t> fail_streak_;
    std::vector<std::int64_t>  ejected_until_ns_;
    BalancerConfig             cfg_{};
    mutable std::uint64_t      rr_ = 0;            // P2C sampling cursor
};

} // namespace ge::lb
