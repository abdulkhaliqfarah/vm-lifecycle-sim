// CLI front end for the VM lifecycle simulator.
//
//   sim --preset baseline
//   sim --preset prefetch        image cache prefetch: nothing starts cold
//   sim --preset slim-image      slimmer guest image: faster Boot + Init
//   sim --preset parallel-net    network attach overlaps Boot/Init
//   sim --preset all             every optimization
//   sim --num-vms 256 --host-slots 16 --cold-fraction 0.5 --seed 7
//   sim --preset baseline --trace  trace.csv   per-VM, per-phase spans
//
// Each run prints per-phase mean/p50/p90/p99 and the share of total startup
// time each phase accounts for, plus total-startup percentiles split by
// cold-image vs warm-image VMs. Same seed = identical numbers.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vmsim/simulator.hpp"

using namespace vmsim;

namespace {

void apply_preset(const char* name, SimConfig& cfg) {
    if (std::strcmp(name, "baseline") == 0) return;
    if (std::strcmp(name, "prefetch") == 0) {
        cfg.cold_image_fraction = 0.0;
        return;
    }
    if (std::strcmp(name, "slim-image") == 0) {
        cfg.boot_min_ns = 300'000'000;  cfg.boot_max_ns = 900'000'000;
        cfg.init_min_ns = 200'000'000;  cfg.init_max_ns = 700'000'000;
        return;
    }
    if (std::strcmp(name, "parallel-net") == 0) {
        // Modelled in the report as Network removed from the critical path.
        cfg.network_min_ns = 0;
        cfg.network_max_ns = 0;
        return;
    }
    if (std::strcmp(name, "all") == 0) {
        apply_preset("prefetch", cfg);
        apply_preset("slim-image", cfg);
        apply_preset("parallel-net", cfg);
        return;
    }
    std::fprintf(stderr, "unknown preset: %s\n", name);
    std::exit(2);
}

void print_report(const char* label, const SimConfig& cfg) {
    SimResult result = run_simulation(cfg);
    StartupStats s = summarize(result);
    std::printf("== %s ==\n", label);
    std::printf("vms %lld  slots %u  cold %.0f%%  makespan %s\n",
                static_cast<long long>(s.count), cfg.host_slots,
                cfg.cold_image_fraction * 100.0, fmt_ms(s.makespan_ns).c_str());
    std::printf("startup total   mean %s  p50 %s  p90 %s  p99 %s  max %s\n",
                fmt_ms(s.mean_ns).c_str(), fmt_ms(s.p50_ns).c_str(),
                fmt_ms(s.p90_ns).c_str(), fmt_ms(s.p99_ns).c_str(),
                fmt_ms(s.max_ns).c_str());
    std::printf("  cold-image p50 %s   warm-image p50 %s   (%lld cold, %lld warm)\n",
                fmt_ms(s.cold_p50_ns).c_str(), fmt_ms(s.warm_p50_ns).c_str(),
                static_cast<long long>(s.cold_count),
                static_cast<long long>(s.warm_count));
    std::printf("%-12s %12s %12s %12s %8s\n", "phase", "mean", "p50", "p99",
                "share");
    for (const auto& ps : s.phases) {
        std::printf("%-12s %12s %12s %12s %7.1f%%\n", phase_name(ps.phase),
                    fmt_ms(ps.mean_ns).c_str(), fmt_ms(ps.p50_ns).c_str(),
                    fmt_ms(ps.p99_ns).c_str(), ps.share_of_startup * 100.0);
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    SimConfig cfg;
    const char* preset = "baseline";
    const char* trace_path = nullptr;
    bool compare = false;

    for (int i = 1; i < argc; ++i) {
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--preset") == 0) preset = need("--preset");
        else if (std::strcmp(argv[i], "--num-vms") == 0)
            cfg.num_vms = static_cast<std::uint32_t>(std::atoi(need("--num-vms")));
        else if (std::strcmp(argv[i], "--host-slots") == 0)
            cfg.host_slots = static_cast<std::uint32_t>(std::atoi(need("--host-slots")));
        else if (std::strcmp(argv[i], "--cold-fraction") == 0)
            cfg.cold_image_fraction = std::atof(need("--cold-fraction"));
        else if (std::strcmp(argv[i], "--seed") == 0)
            cfg.seed = std::strtoull(need("--seed"), nullptr, 10);
        else if (std::strcmp(argv[i], "--arrival-gap-ms") == 0)
            cfg.arrival_gap_ns = static_cast<std::int64_t>(std::atoll(need("--arrival-gap-ms"))) * 1'000'000;
        else if (std::strcmp(argv[i], "--trace") == 0) trace_path = need("--trace");
        else if (std::strcmp(argv[i], "--compare") == 0) compare = true;
        else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    if (compare) {
        const char* presets[] = {"baseline", "prefetch", "slim-image",
                                 "parallel-net", "all"};
        for (const char* p : presets) {
            SimConfig c = cfg;
            apply_preset(p, c);
            print_report(p, c);
        }
        return 0;
    }

    SimConfig c = cfg;
    apply_preset(preset, c);
    print_report(preset, c);

    if (trace_path != nullptr) {
        SimResult result = run_simulation(c);
        FILE* out = std::fopen(trace_path, "w");
        if (out == nullptr) {
            std::perror("fopen trace");
            return 1;
        }
        std::fprintf(out, "vm_id,cold_image,submitted_ns,phase,start_ns,end_ns,duration_ns\n");
        for (const auto& vm : result.vms) {
            for (const auto& span : vm.spans) {
                std::fprintf(out, "%llu,%d,%lld,%s,%lld,%lld,%lld\n",
                             static_cast<unsigned long long>(vm.id),
                             vm.cold_image ? 1 : 0,
                             static_cast<long long>(vm.submitted_ns),
                             phase_name(span.phase),
                             static_cast<long long>(span.start_ns),
                             static_cast<long long>(span.end_ns),
                             static_cast<long long>(span.duration_ns()));
            }
            std::fprintf(out, "%llu,%d,%lld,Ready,%lld,%lld,0\n",
                         static_cast<unsigned long long>(vm.id),
                         vm.cold_image ? 1 : 0,
                         static_cast<long long>(vm.submitted_ns),
                         static_cast<long long>(vm.ready_ns),
                         static_cast<long long>(vm.ready_ns));
        }
        std::fclose(out);
        std::printf("trace written to %s (%zu vms)\n", trace_path,
                    result.vms.size());
    }
    return 0;
}
