// Differential-test driver: reads a JSON-free text config on the first line
// of stdin, runs the simulator, and prints the same per-VM trace format as
// `sim --trace`, so scripts/model.py can diff the two implementations.
//
// First stdin line: seed num_vms host_slots cold_fraction arrival_gap_ns
//                   queue_min queue_max
//                   cold_lo cold_hi warm_lo warm_hi
//                   create_lo create_hi boot_lo boot_hi
//                   init_lo init_hi net_lo net_hi
// Output: the trace rows (vm_id,cold,submitted,phase,start,end,duration).
#include <cstdio>
#include <iostream>

#include "vmsim/simulator.hpp"

int main() {
    vmsim::SimConfig cfg;
    double cold_fraction = 0.0;
    if (!(std::cin >> cfg.seed >> cfg.num_vms >> cfg.host_slots >>
          cold_fraction >> cfg.arrival_gap_ns >> cfg.queue_min_ns >>
          cfg.queue_max_ns >> cfg.image_cold_min_ns >> cfg.image_cold_max_ns >>
          cfg.image_warm_min_ns >> cfg.image_warm_max_ns >> cfg.create_min_ns >>
          cfg.create_max_ns >> cfg.boot_min_ns >> cfg.boot_max_ns >>
          cfg.init_min_ns >> cfg.init_max_ns >> cfg.network_min_ns >>
          cfg.network_max_ns)) {
        std::fprintf(stderr, "bad config line\n");
        return 1;
    }
    cfg.cold_image_fraction = cold_fraction;

    vmsim::SimResult result = vmsim::run_simulation(cfg);
    for (const auto& vm : result.vms) {
        for (const auto& span : vm.spans) {
            std::printf("%llu,%d,%lld,%s,%lld,%lld,%lld\n",
                        static_cast<unsigned long long>(vm.id),
                        vm.cold_image ? 1 : 0,
                        static_cast<long long>(vm.submitted_ns),
                        vmsim::phase_name(span.phase),
                        static_cast<long long>(span.start_ns),
                        static_cast<long long>(span.end_ns),
                        static_cast<long long>(span.duration_ns()));
        }
        std::printf("%llu,%d,%lld,Ready,%lld,%lld,0\n",
                    static_cast<unsigned long long>(vm.id),
                    vm.cold_image ? 1 : 0,
                    static_cast<long long>(vm.submitted_ns),
                    static_cast<long long>(vm.ready_ns),
                    static_cast<long long>(vm.ready_ns));
    }
    return 0;
}
