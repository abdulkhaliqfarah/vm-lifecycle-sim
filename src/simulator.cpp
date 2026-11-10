#include "vmsim/simulator.hpp"

#include <algorithm>
#include <cstdio>

namespace vmsim {
namespace {

// splitmix64, same family as the limit-order-book feed generator: the draw
// for (vm, phase) depends only on the master seed, the vm id and the phase,
// never on execution order -- that is what makes runs replayable.
std::uint64_t splitmix(std::uint64_t x) {
    std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

std::int64_t draw_range(std::uint64_t seed, std::uint64_t vm, Phase phase,
                        std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) return lo;
    std::uint64_t key = seed ^ (vm * 0x9e3779b97f4a7c15ULL) ^
                        (static_cast<std::uint64_t>(phase) *
                         0xbf58476d1ce4e5b9ULL);
    std::uint64_t span = static_cast<std::uint64_t>(hi - lo);
    return lo + static_cast<std::int64_t>(splitmix(key) % (span + 1));
}

bool is_cold(std::uint64_t seed, std::uint64_t vm, double fraction) {
    if (fraction <= 0.0) return false;
    if (fraction >= 1.0) return true;
    std::uint64_t key = seed ^ (vm * 0x94d049bb133111ebULL) ^ 0x2545F4914F6CDD1DULL;
    double u = static_cast<double>(splitmix(key) >> 11) /
               static_cast<double>(1ULL << 53);
    return u < fraction;
}

}  // namespace

SimResult run_simulation(const SimConfig& cfg) {
    SimResult result;
    result.vms.reserve(cfg.num_vms);

    // Slot availability times, earliest first. A VM takes the earliest free
    // slot; its QueueWait ends when that slot opens (never before submit).
    std::priority_queue<std::int64_t, std::vector<std::int64_t>,
                        std::greater<std::int64_t>>
        slot_free_at;
    if (cfg.host_slots > 0)
        for (std::uint32_t i = 0; i < cfg.host_slots; ++i)
            slot_free_at.push(0);

    for (std::uint32_t v = 0; v < cfg.num_vms; ++v) {
        const std::uint64_t id = v + 1;
        VmRecord rec;
        rec.id = id;
        rec.submitted_ns = static_cast<std::int64_t>(v) * cfg.arrival_gap_ns;
        rec.cold_image = is_cold(cfg.seed, id, cfg.cold_image_fraction);

        std::int64_t t = rec.submitted_ns;

        // QueueWait: contention wait for a host slot, plus any configured
        // scheduler latency once the slot is granted.
        std::int64_t slot_at = t;
        if (cfg.host_slots > 0) {
            slot_at = std::max(t, slot_free_at.top());
            slot_free_at.pop();
        }
        std::int64_t sched = draw_range(cfg.seed, id, Phase::QueueWait,
                                        cfg.queue_min_ns, cfg.queue_max_ns);
        std::int64_t pipeline_start = slot_at + sched;
        if (pipeline_start > t) {
            rec.spans.push_back(
                PhaseSpan{Phase::QueueWait, t, pipeline_start});
            t = pipeline_start;
        }

        auto run_phase = [&](Phase p, std::int64_t lo, std::int64_t hi) {
            std::int64_t d = draw_range(cfg.seed, id, p, lo, hi);
            rec.spans.push_back(PhaseSpan{p, t, t + d});
            t += d;
        };

        if (rec.cold_image)
            run_phase(Phase::ImageFetch, cfg.image_cold_min_ns,
                      cfg.image_cold_max_ns);
        else
            run_phase(Phase::ImageFetch, cfg.image_warm_min_ns,
                      cfg.image_warm_max_ns);
        run_phase(Phase::Create, cfg.create_min_ns, cfg.create_max_ns);
        run_phase(Phase::Boot, cfg.boot_min_ns, cfg.boot_max_ns);
        run_phase(Phase::Init, cfg.init_min_ns, cfg.init_max_ns);
        run_phase(Phase::Network, cfg.network_min_ns, cfg.network_max_ns);

        rec.ready_ns = t;
        if (cfg.host_slots > 0) slot_free_at.push(t);
        result.vms.push_back(std::move(rec));
    }

    std::int64_t last_ready = 0;
    for (const auto& vm : result.vms)
        last_ready = std::max(last_ready, vm.ready_ns);
    result.makespan_ns = result.vms.empty() ? 0 : last_ready;
    return result;
}

// ---------------- aggregation ----------------

namespace {

std::int64_t percentile(std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(p * sorted.size());
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

void fill_stats(std::vector<std::int64_t> values, std::int64_t& mean,
                std::int64_t& p50, std::int64_t& p90, std::int64_t& p99,
                std::int64_t& maxv, std::int64_t& total) {
    total = 0;
    maxv = 0;
    for (auto v : values) {
        total += v;
        maxv = std::max(maxv, v);
    }
    mean = values.empty() ? 0
                          : total / static_cast<std::int64_t>(values.size());
    std::sort(values.begin(), values.end());
    p50 = percentile(values, 0.50);
    p90 = percentile(values, 0.90);
    p99 = percentile(values, 0.99);
}

}  // namespace

StartupStats summarize(const SimResult& result) {
    StartupStats stats;
    stats.makespan_ns = result.makespan_ns;

    std::vector<std::int64_t> totals, cold_totals, warm_totals;
    std::vector<std::int64_t> phase_values[kNumPhases];
    std::int64_t grand_total = 0;

    for (const auto& vm : result.vms) {
        if (!vm.ready()) continue;
        std::int64_t total = vm.total_startup_ns();
        totals.push_back(total);
        grand_total += total;
        (vm.cold_image ? cold_totals : warm_totals).push_back(total);
        for (std::size_t i = 0; i < kNumPhases; ++i)
            phase_values[i].push_back(
                vm.phase_ns(static_cast<Phase>(i)));
    }

    stats.count = static_cast<std::int64_t>(totals.size());
    std::int64_t total_sum = 0;
    fill_stats(totals, stats.mean_ns, stats.p50_ns, stats.p90_ns,
               stats.p99_ns, stats.max_ns, total_sum);

    stats.phases.resize(kNumPhases);
    for (std::size_t i = 0; i < kNumPhases; ++i) {
        PhaseStats& ps = stats.phases[i];
        ps.phase = static_cast<Phase>(i);
        ps.count = static_cast<std::int64_t>(phase_values[i].size());
        fill_stats(phase_values[i], ps.mean_ns, ps.p50_ns, ps.p90_ns,
                   ps.p99_ns, ps.max_ns, ps.total_ns);
        ps.share_of_startup =
            grand_total > 0
                ? static_cast<double>(ps.total_ns) / grand_total
                : 0.0;
    }

    stats.cold_count = static_cast<std::int64_t>(cold_totals.size());
    stats.warm_count = static_cast<std::int64_t>(warm_totals.size());
    {
        std::int64_t m, p90, p99, mx, tot;
        fill_stats(cold_totals, m, stats.cold_p50_ns, p90, p99, mx, tot);
        fill_stats(warm_totals, m, stats.warm_p50_ns, p90, p99, mx, tot);
    }
    return stats;
}

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::QueueWait: return "QueueWait";
        case Phase::ImageFetch: return "ImageFetch";
        case Phase::Create: return "Create";
        case Phase::Boot: return "Boot";
        case Phase::Init: return "Init";
        case Phase::Network: return "Network";
        case Phase::Ready: return "Ready";
    }
    return "?";
}

std::string fmt_ms(std::int64_t ns) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f ms",
                  static_cast<double>(ns) / 1e6);
    return buf;
}

}  // namespace vmsim
