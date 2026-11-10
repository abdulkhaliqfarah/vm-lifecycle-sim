# Design notes

## The problem this models

A cloud VM start is not one operation, it is a pipeline across two machines'
worth of responsibility:

1. **QueueWait** — the request waits for host capacity / a scheduler slot.
2. **ImageFetch** — the VM image lands on the host. Near-instant when the
   host already caches the image, seconds when it must be pulled cold.
3. **Create** — the hypervisor creates the domain: config, devices, memory.
4. **Boot** — firmware + guest kernel boot. Inside the guest.
5. **Init** — guest userspace: systemd/cloud-init, agents, daemon warmup.
   Inside the guest.
6. **Network** — virtual NIC and network namespace attach, IP assignment.

Boot and Init are **container-side** work (they run inside the VM container /
guest). QueueWait, ImageFetch, Create and Network are **host-side** work
(platform control plane + hypervisor). The split decides who can fix a slow
phase: guest time shrinks with image engineering, host time shrinks with
scheduling, caching and placement.

## Scheduling model

- A VM occupies one host slot from the end of its QueueWait until it is
  Ready. Slots are granted FIFO in submission order, implemented as a
  min-heap of slot-free times.
- Phases inside the pipeline run back-to-back. There is no per-phase
  resource contention inside a host in this model; see limitations.
- Arrivals are a fixed-gap process (`arrival_gap_ns`), first VM at t=0.

## Why this structure

**Deterministic draws, seeded per (vm, phase).** Every duration comes from a
splitmix64 hash of (seed, vm id, phase) mapped into a configured range. A
run is therefore a pure function of its config: same seed, bit-for-bit same
trace. That is what makes the differential test against the Python model
meaningful, and what makes benchmark comparisons between presets fair — the
baseline and an optimization preset see the *same* random VMs, so a delta is
the optimization, not noise.

**Aggregation is separate from simulation.** `run_simulation` returns raw
per-VM spans; `summarize` computes percentiles and phase shares. The CLI, the
tests and the differential model all consume the same raw spans.

**Percentiles use the index method** (sorted values, idx = p * n clamped),
the same convention as the limit order book replay tool.

## What the numbers say (128 VMs, 16 slots, seed 1)

Baseline startup p50 **4542 ms** per VM when capacity is unlimited
(32 VMs, no queueing) — with Boot (2.1 s mean) and Init (1.7 s mean) the two
largest phases, and cold-image VMs paying ~1.6 s extra in ImageFetch
(cold p50 6438 ms vs warm p50 3957 ms total).

Under a scale-out burst (128 VMs onto 16 slots) the picture flips:
**QueueWait is 77% of total startup time** — capacity, not guest speed,
dominates. Optimizations still help, because every millisecond cut from the
pipeline is a millisecond a slot frees up sooner: slim-image cuts mean
startup from 21.2 s to 8.5 s, prefetch only to 19.4 s, and all three
together to 6.1 s.

## Optimization presets

- **prefetch** — image cache prefetch / pre-warmed hosts: no VM starts with
  a cold image. Removes the ImageFetch tail entirely.
- **slim-image** — a smaller, pre-baked guest image (snapshot-style boot,
  deferred cloud-init work): Boot 1.2–3.2 s → 0.3–0.9 s, Init 0.8–2.6 s →
  0.2–0.7 s.
- **parallel-net** — network attach overlapped with Boot/Init instead of
  serialized after them: Network leaves the critical path.
- **all** — the three combined.

## Limitations

- Phases inside the pipeline are serialized; apart from parallel-net, no
  overlap is modelled (real platforms overlap ImageFetch with Create, and
  some attach networking during boot).
- No host CPU contention: two VMs booting on one host do not slow each
  other in this model. Slots are the only shared resource.
- One instrument: one VM type, one image family, one host pool.
- Duration ranges are plausible defaults, not measurements from a specific
  cloud. The point is the breakdown structure and the relative effect of
  each optimization, not the absolute numbers.
