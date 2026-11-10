#pragma once

#include <cstdint>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace vmsim {

// Phase windows of one VM start. Boot/Init are container-side (run inside the
// VM's guest: guest kernel boot, cloud-init, daemon warmup); the rest are
// host-side (platform control plane and hypervisor work). The split matters:
// cutting container-side latency needs guest image work, cutting host-side
// latency needs scheduling/placement work.
enum class Phase : std::uint8_t {
    QueueWait = 0,  // waiting for host capacity / scheduler slot
    ImageFetch,     // fetching the VM image (cold cache vs cached on host)
    Create,         // hypervisor domain create
    Boot,           // guest kernel + firmware boot
    Init,           // guest init / cloud-init / agent warmup
    Network,        // virtual NIC + network namespace attach
    Ready           // terminal pseudo-phase: timestamp of Ready
};
inline constexpr std::size_t kNumPhases = 6;  // excluding Ready

struct PhaseSpan {
    Phase phase;
    std::int64_t start_ns;
    std::int64_t end_ns;
    std::int64_t duration_ns() const { return end_ns - start_ns; }
};

struct VmRecord {
    std::uint64_t id;
    std::int64_t submitted_ns;
    std::vector<PhaseSpan> spans;  // completed phases, in order
    std::int64_t ready_ns = -1;    // -1 while not ready
    bool cold_image = false;

    std::int64_t total_startup_ns() const {
        return ready_ns < 0 ? -1 : ready_ns - submitted_ns;
    }
    std::int64_t phase_ns(Phase p) const {
        for (const auto& s : spans)
            if (s.phase == p) return s.duration_ns();
        return 0;
    }
    bool ready() const { return ready_ns >= 0; }
};

// Deterministic config: every duration is drawn from a splitmix64 stream
// seeded by (seed, vm id, phase), so a run is a pure function of the config
// and replays bit-for-bit. Ranges are uniform in [min_ns, max_ns].
struct SimConfig {
    std::uint64_t seed = 1;
    std::uint32_t num_vms = 64;
    // Host capacity: how many VMs may be in the host-side start pipeline at
    // once. Excess VMs accumulate QueueWait. 0 = unlimited.
    std::uint32_t host_slots = 8;
    // Fraction of VMs whose image is not cached on the assigned host.
    double cold_image_fraction = 0.3;
    // Arrival process: VMs are submitted every arrival_gap_ns; the first at 0.
    std::int64_t arrival_gap_ns = 0;

    std::int64_t queue_min_ns = 0, queue_max_ns = 0;  // only when slots full
    std::int64_t image_cold_min_ns = 900'000'000, image_cold_max_ns = 2'500'000'000;
    std::int64_t image_warm_min_ns = 40'000'000, image_warm_max_ns = 120'000'000;
    std::int64_t create_min_ns = 150'000'000, create_max_ns = 400'000'000;
    std::int64_t boot_min_ns = 1'200'000'000, boot_max_ns = 3'200'000'000;
    std::int64_t init_min_ns = 800'000'000, init_max_ns = 2'600'000'000;
    std::int64_t network_min_ns = 60'000'000, network_max_ns = 220'000'000;
};

struct SimResult {
    std::vector<VmRecord> vms;
    std::int64_t makespan_ns = 0;  // last Ready minus earliest submit
};

// Runs the whole simulation and returns per-VM phase spans.
// Scheduling model: a VM occupies a host slot from the end of its QueueWait
// until it becomes Ready; slots are granted in submission order (FIFO).
// Phases inside the pipeline run back-to-back with no gaps.
SimResult run_simulation(const SimConfig& cfg);

// ---------------- aggregation ----------------

struct PhaseStats {
    Phase phase;
    std::int64_t count = 0;
    std::int64_t total_ns = 0;
    std::int64_t mean_ns = 0;
    std::int64_t p50_ns = 0, p90_ns = 0, p99_ns = 0;
    std::int64_t max_ns = 0;
    double share_of_startup = 0.0;  // total / sum of all VM total startup times
};

struct StartupStats {
    std::int64_t count = 0;
    std::int64_t mean_ns = 0;
    std::int64_t p50_ns = 0, p90_ns = 0, p99_ns = 0;
    std::int64_t max_ns = 0;
    std::int64_t makespan_ns = 0;
    std::vector<PhaseStats> phases;  // QueueWait .. Network, in order
    std::int64_t cold_count = 0, warm_count = 0;
    std::int64_t cold_p50_ns = 0, warm_p50_ns = 0;  // total startup split
};

StartupStats summarize(const SimResult& result);

const char* phase_name(Phase p);
std::string fmt_ms(std::int64_t ns);  // "1234.5 ms"

}  // namespace vmsim
