# VM Lifecycle Simulator — Where VM Startup Time Goes

A C++20 simulator of the cloud VM startup pipeline: QueueWait, ImageFetch,
Create, Boot, Init, Network. It answers the question behind a real intern
project — **reducing VM startup latency to speed up scaling for AI
workloads** — by breaking total startup time into its phases and measuring
what each optimization actually buys.

Boot and Init run inside the guest (container-side); QueueWait, ImageFetch,
Create and Network are host-side platform work. The simulator keeps that
split visible in every report, because the fix for guest time is image
engineering and the fix for host time is scheduling and caching.

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/sim --preset baseline            # one scenario
./build/sim --compare --num-vms 128 --host-slots 16   # all presets
./build/sim --preset baseline --trace trace.csv       # per-VM phase spans
ctest --test-dir build                    # 125 scenario checks
python3 scripts/model.py - '<config json>'  # differential vs Python model
```

## What it found (seed 1)

Single VM stream, unlimited capacity — startup p50 **4.5 s**:

| Phase | Mean | Share |
|---|---|---|
| Boot | 2149 ms | guest kernel + firmware |
| Init | 1701 ms | guest userspace / cloud-init |
| ImageFetch | 505 ms | 0.9–2.5 s cold, 40–120 ms cached |
| Create | 277 ms | hypervisor domain create |
| Network | 144 ms | NIC + namespace attach |

Scale-out burst, 128 VMs onto 16 host slots — the bottleneck flips:
**QueueWait is 77% of total startup time.** Capacity dominates, and every
phase cut still compounds through the queue:

| Preset | Mean startup | What it does |
|---|---|---|
| baseline | 21.2 s | — |
| prefetch | 19.4 s | no cold image fetches |
| slim-image | 8.5 s | Boot 0.3–0.9 s, Init 0.2–0.7 s |
| parallel-net | 20.5 s | Network off the critical path |
| all | 6.1 s | the three combined |

Cold-image VMs are measurably slower end to end: p50 6.4 s vs 4.0 s warm in
the unqueued stream.

## Design

- **Deterministic.** Every phase duration is a splitmix64 draw seeded by
  (seed, VM id, phase). Same config, bit-for-bit same trace — preset
  comparisons are apples-to-apples and the Python differential model can
  diff every span.
- **Slots are the only shared resource.** A VM holds a host slot from the
  end of QueueWait until Ready; grants are FIFO via a min-heap of
  slot-free times.
- **Simulation and statistics are separate.** The engine emits raw per-VM
  phase spans; percentiles and phase shares are computed from the spans.

Details, trade-offs and limitations: [DESIGN.md](DESIGN.md).

## Verification

- 125 scenario checks: phase order and contiguity, FIFO slot queueing,
  cold-vs-warm image cost, arrival gaps, determinism, share math.
- Differential test against an independent naive Python model: 48 VMs,
  336 phase spans, exact agreement.
