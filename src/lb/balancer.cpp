#include "ge/lb/balancer.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

#include "ge/core/bits.hpp"

namespace ge::lb {

void Balancer::configure(std::vector<BackendConfig> backends, const BalancerConfig& cfg) {
    backends_ = std::move(backends);
    cfg_      = cfg;
    stats_.assign(backends_.size(), BackendStats{});
    fail_streak_.assign(backends_.size(), 0);
    ejected_until_ns_.assign(backends_.size(), 0);

    ring_.clear();
    ring_.reserve(backends_.size() * cfg_.virtual_nodes);
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        // Virtual node count scales with weight, so a backend with twice the
        // capacity owns twice the key space without any runtime bookkeeping.
        const std::uint32_t vnodes = cfg_.virtual_nodes * std::max(1u, backends_[i].weight);
        char key[128];
        for (std::uint32_t v = 0; v < vnodes; ++v) {
            const int n = std::snprintf(key, sizeof key, "%s:%u#%u",
                                        backends_[i].host.c_str(),
                                        unsigned(backends_[i].port), unsigned(v));
            // FNV-1a alone leaves the high bits correlated for near-identical
            // keys ("host:port#0", "host:port#1", ...), which clusters virtual
            // nodes on the ring and skews the split by tens of percent. Running
            // it through a finaliser decorrelates the bits the ring sorts on.
            ring_.push_back({core::mix64(core::fnv1a(key, std::size_t(n))), std::uint32_t(i)});
        }
    }
    std::sort(ring_.begin(), ring_.end(),
              [](const RingNode& a, const RingNode& b) { return a.hash < b.hash; });
}

int Balancer::ring_lookup(std::uint32_t session) const noexcept {
    if (ring_.empty()) return -1;
    const std::uint64_t h = core::mix64(session);
    // First node with hash >= h, wrapping to the front.
    const auto it = std::lower_bound(ring_.begin(), ring_.end(), h,
                                     [](const RingNode& n, std::uint64_t v) { return n.hash < v; });
    return int((it == ring_.end() ? ring_.front() : *it).backend);
}

bool Balancer::available(int idx, std::int64_t now_ns) const noexcept {
    const auto i = std::size_t(idx);
    return stats_[i].healthy && now_ns >= ejected_until_ns_[i];
}

double Balancer::score(int idx) const noexcept {
    const BackendStats& s = stats_[std::size_t(idx)];
    // Queue depth dominates; latency breaks ties. The +1 keeps a backend with
    // zero measured RTT (never probed) from looking infinitely attractive.
    const double rtt_ms = double(s.rtt_ewma_ns ? s.rtt_ewma_ns : 1'000'000) / 1e6;
    return double(s.inflight) * 1.0 + rtt_ms * 0.25 + double(s.reported_load_pct) * 0.01;
}

int Balancer::pick(std::uint32_t session, std::int64_t now_ns) const noexcept {
    if (backends_.empty()) return -1;

    if (cfg_.session_affinity && session != 0) {
        const int home = ring_lookup(session);
        if (home >= 0 && available(home, now_ns)) return home;
        // The session's home is ejected. Walk the ring forward to the next
        // healthy node: that is the standard consistent-hash failover and it
        // sends every orphaned session from one backend to the same successor
        // rather than scattering them, which keeps the remap predictable.
        if (home >= 0 && !ring_.empty()) {
            const std::uint64_t h = core::mix64(session);
            auto it = std::lower_bound(ring_.begin(), ring_.end(), h,
                                       [](const RingNode& n, std::uint64_t v) { return n.hash < v; });
            for (std::size_t step = 0; step < ring_.size(); ++step) {
                if (it == ring_.end()) it = ring_.begin();
                const int cand = int(it->backend);
                if (available(cand, now_ns)) return cand;
                ++it;
            }
        }
        return -1;
    }

    // Power of two choices over the healthy set.
    const std::size_t n = backends_.size();
    int best = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        int cand = -1;
        for (std::size_t step = 0; step < n; ++step) {
            const int i = int((rr_++ ) % n);
            if (available(i, now_ns)) { cand = i; break; }
        }
        if (cand < 0) continue;
        if (best < 0 || score(cand) < score(best)) best = cand;
    }
    return best;
}

void Balancer::on_probe_sent(int idx) noexcept {
    if (idx < 0) return;
    ++stats_[std::size_t(idx)].probes_sent;
}

void Balancer::on_probe_reply(int idx, std::int64_t rtt_ns, std::uint16_t load_pct) noexcept {
    if (idx < 0) return;
    BackendStats& s = stats_[std::size_t(idx)];
    ++s.probes_ok;
    s.last_rtt_ns = rtt_ns;
    s.rtt_ewma_ns = s.rtt_ewma_ns == 0
        ? rtt_ns
        : std::int64_t(cfg_.rtt_ewma_alpha * double(rtt_ns)
                     + (1.0 - cfg_.rtt_ewma_alpha) * double(s.rtt_ewma_ns));
    s.reported_load_pct = load_pct;
    s.healthy = true;
    fail_streak_[std::size_t(idx)] = 0;
}

void Balancer::on_probe_timeout(int idx, std::int64_t now_ns) noexcept {
    if (idx < 0) return;
    BackendStats& s = stats_[std::size_t(idx)];
    ++s.probes_failed;
    // One slow probe is noise; a streak is a signal. Ejecting on the first
    // failure would take a backend out of rotation for a single dropped packet.
    if (++fail_streak_[std::size_t(idx)] >= cfg_.eject_after_failures && s.healthy) {
        s.healthy = false;
        ++s.ejections;
        ejected_until_ns_[std::size_t(idx)] = now_ns + cfg_.eject_cooldown_ns;
    }
}

void Balancer::on_connect_failed(int idx, std::int64_t now_ns) noexcept {
    on_probe_timeout(idx, now_ns);
}

void Balancer::on_session_opened(int idx) noexcept {
    if (idx < 0) return;
    ++stats_[std::size_t(idx)].sessions_assigned;
    ++stats_[std::size_t(idx)].inflight;
}

void Balancer::on_session_closed(int idx) noexcept {
    if (idx < 0) return;
    BackendStats& s = stats_[std::size_t(idx)];
    if (s.inflight) --s.inflight;
}

void Balancer::tick(std::int64_t now_ns) noexcept {
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        if (!stats_[i].healthy && now_ns >= ejected_until_ns_[i]) {
            // Half-open: readmit and let the next probe decide. If it fails the
            // streak counter is already at the threshold, so it is ejected again
            // immediately rather than absorbing three more failures.
            stats_[i].healthy = true;
            fail_streak_[i] = cfg_.eject_after_failures - 1;
        }
    }
}

std::size_t Balancer::healthy_count() const noexcept {
    return std::size_t(std::count_if(stats_.begin(), stats_.end(),
                                     [](const BackendStats& s) { return s.healthy; }));
}

std::vector<double> Balancer::key_space_share() const {
    std::vector<double> share(backends_.size(), 0.0);
    if (ring_.empty()) return share;
    const double total = 18446744073709551616.0;   // 2^64
    for (std::size_t i = 0; i < ring_.size(); ++i) {
        const std::uint64_t prev = (i == 0) ? ring_.back().hash : ring_[i - 1].hash;
        const double span = (i == 0)
            ? double(ring_[0].hash) + (total - double(prev))
            : double(ring_[i].hash - prev);
        share[ring_[i].backend] += span / total;
    }
    return share;
}

} // namespace ge::lb
