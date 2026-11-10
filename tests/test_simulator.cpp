// Scenario tests for the VM lifecycle simulator. Each test pins one rule:
// phase order and contiguity, FIFO slot queueing, cold vs warm image cost,
// determinism, and the aggregation math. Exits non-zero on any failure.
#include <cstdio>
#include <vector>

#include "vmsim/simulator.hpp"

using namespace vmsim;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                       \
    do {                                                  \
        ++checks;                                         \
        if (!(cond)) {                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__,     \
                        __LINE__, #cond);                 \
            ++failures;                                   \
        }                                                 \
    } while (0)

static SimConfig flat_config() {
    // Zero-width ranges: every duration is exact, so tests can assert
    // precise timelines without depending on the RNG.
    SimConfig cfg;
    cfg.num_vms = 4;
    cfg.host_slots = 0;  // unlimited
    cfg.cold_image_fraction = 0.0;
    cfg.image_warm_min_ns = cfg.image_warm_max_ns = 100;
    cfg.create_min_ns = cfg.create_max_ns = 200;
    cfg.boot_min_ns = cfg.boot_max_ns = 300;
    cfg.init_min_ns = cfg.init_max_ns = 400;
    cfg.network_min_ns = cfg.network_max_ns = 50;
    return cfg;
}

static void test_phase_order_and_total() {
    SimResult r = run_simulation(flat_config());
    CHECK(r.vms.size() == 4);
    const VmRecord& vm = r.vms[0];
    CHECK(vm.spans.size() == 5);  // no QueueWait when slots are unlimited
    CHECK(vm.spans[0].phase == Phase::ImageFetch);
    CHECK(vm.spans[1].phase == Phase::Create);
    CHECK(vm.spans[2].phase == Phase::Boot);
    CHECK(vm.spans[3].phase == Phase::Init);
    CHECK(vm.spans[4].phase == Phase::Network);
    // Spans are contiguous: each starts where the previous ended.
    for (std::size_t i = 1; i < vm.spans.size(); ++i)
        CHECK(vm.spans[i].start_ns == vm.spans[i - 1].end_ns);
    CHECK(vm.ready_ns == 100 + 200 + 300 + 400 + 50);
    CHECK(vm.total_startup_ns() == 1050);
}

static void test_slot_queueing_fifo() {
    SimConfig cfg = flat_config();
    cfg.host_slots = 1;  // one VM in the pipeline at a time
    cfg.num_vms = 3;
    SimResult r = run_simulation(cfg);
    // VM1 starts at 0, ready at 1050. VM2 waits the full 1050, VM3 waits 2100.
    CHECK(r.vms[0].phase_ns(Phase::QueueWait) == 0);
    CHECK(r.vms[1].phase_ns(Phase::QueueWait) == 1050);
    CHECK(r.vms[2].phase_ns(Phase::QueueWait) == 2100);
    CHECK(r.vms[2].ready_ns == 3150);
    CHECK(r.makespan_ns == 3150);
}

static void test_cold_image_costs_more() {
    SimConfig cfg = flat_config();
    cfg.cold_image_fraction = 1.0;
    cfg.image_cold_min_ns = cfg.image_cold_max_ns = 5000;
    SimResult r = run_simulation(cfg);
    CHECK(r.vms[0].cold_image);
    CHECK(r.vms[0].phase_ns(Phase::ImageFetch) == 5000);
    CHECK(r.vms[0].total_startup_ns() == 5000 + 200 + 300 + 400 + 50);
}

static void test_determinism() {
    SimConfig cfg;  // default ranges, real randomness
    cfg.num_vms = 32;
    SimResult a = run_simulation(cfg);
    SimResult b = run_simulation(cfg);
    CHECK(a.vms.size() == b.vms.size());
    for (std::size_t i = 0; i < a.vms.size(); ++i) {
        CHECK(a.vms[i].ready_ns == b.vms[i].ready_ns);
        CHECK(a.vms[i].cold_image == b.vms[i].cold_image);
        CHECK(a.vms[i].spans.size() == b.vms[i].spans.size());
    }
    SimConfig other = cfg;
    other.seed = 99;
    SimResult c = run_simulation(other);
    bool any_diff = false;
    for (std::size_t i = 0; i < a.vms.size(); ++i)
        if (a.vms[i].ready_ns != c.vms[i].ready_ns) any_diff = true;
    CHECK(any_diff);
}

static void test_arrival_gap() {
    SimConfig cfg = flat_config();
    cfg.arrival_gap_ns = 1000;
    SimResult r = run_simulation(cfg);
    CHECK(r.vms[1].submitted_ns == 1000);
    CHECK(r.vms[1].ready_ns == 1000 + 1050);
}

static void test_summary_shares_sum_to_one() {
    SimConfig cfg;
    cfg.num_vms = 16;
    StartupStats s = summarize(run_simulation(cfg));
    CHECK(s.count == 16);
    double share_sum = 0.0;
    for (const auto& ps : s.phases) share_sum += ps.share_of_startup;
    CHECK(share_sum > 0.999 && share_sum < 1.001);
    CHECK(s.cold_count + s.warm_count == 16);
    CHECK(s.p50_ns > 0 && s.p99_ns >= s.p50_ns);
}

int main() {
    test_phase_order_and_total();
    test_slot_queueing_fifo();
    test_cold_image_costs_more();
    test_determinism();
    test_arrival_gap();
    test_summary_shares_sum_to_one();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
