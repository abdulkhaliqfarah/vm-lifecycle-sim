#!/usr/bin/env python3
"""Differential test: a naive Python model of the VM lifecycle sim vs C++.

The model is deliberately dumb -- a plain event simulation written straight
from the scheduling rules (FIFO host slots, back-to-back phases) -- so it is
easy to trust. It reads the C++ per-VM trace CSV, replays the same config
independently, and diffs every VM's Ready time and every phase span.

Config is passed as JSON on the command line, matching the C++ trace run:
  python3 scripts/model.py <trace.csv> '{"seed":1,"num_vms":64,
      "host_slots":8,"cold_fraction":0.3,"arrival_gap_ns":0,
      "queue_min_ns":0,"queue_max_ns":0,
      "image_cold":[900000000,2500000000],"image_warm":[40000000,120000000],
      "create":[150000000,400000000],"boot":[1200000000,3200000000],
      "init":[800000000,2600000000],"network":[60000000,220000000]}'

Exit code 0 = the two implementations agree exactly on every span.
"""

import csv
import json
import subprocess
import sys

MASK = (1 << 64) - 1
PHASES = ["QueueWait", "ImageFetch", "Create", "Boot", "Init", "Network"]
PHASE_IDX = {name: i for i, name in enumerate(PHASES)}


def splitmix(x):
    x = (x + 0x9E3779B97F4A7C15) & MASK
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
    return (z ^ (z >> 31)) & MASK


def draw(seed, vm, phase_idx, lo, hi):
    if hi <= lo:
        return lo
    key = (seed ^ ((vm * 0x9E3779B97F4A7C15) & MASK)
           ^ ((phase_idx * 0xBF58476D1CE4E5B9) & MASK)) & MASK
    return lo + splitmix(key) % (hi - lo + 1)


def is_cold(seed, vm, fraction):
    if fraction <= 0.0:
        return False
    if fraction >= 1.0:
        return True
    key = (seed ^ ((vm * 0x94D049BB133111EB) & MASK)
           ^ 0x2545F4914F6CDD1D) & MASK
    u = (splitmix(key) >> 11) / float(1 << 53)
    return u < fraction


def model(cfg):
    """Returns {vm_id: [(phase, start, end), ...] + [('Ready', t, t)]}."""
    import heapq
    slots = []
    if cfg["host_slots"] > 0:
        slots = [0] * cfg["host_slots"]
        heapq.heapify(slots)
    out = {}
    for v in range(cfg["num_vms"]):
        vm = v + 1
        t = v * cfg["arrival_gap_ns"]
        submitted = t
        spans = []
        slot_at = t
        if cfg["host_slots"] > 0:
            slot_at = max(t, heapq.heappop(slots))
        sched = draw(cfg["seed"], vm, 0, cfg["queue_min_ns"],
                     cfg["queue_max_ns"])
        start = slot_at + sched
        if start > t:
            spans.append(("QueueWait", t, start))
            t = start
        cold = is_cold(cfg["seed"], vm, cfg["cold_fraction"])
        ranges = [
            ("ImageFetch", cfg["image_cold"] if cold else cfg["image_warm"]),
            ("Create", cfg["create"]),
            ("Boot", cfg["boot"]),
            ("Init", cfg["init"]),
            ("Network", cfg["network"]),
        ]
        for name, (lo, hi) in ranges:
            d = draw(cfg["seed"], vm, PHASE_IDX[name], lo, hi)
            spans.append((name, t, t + d))
            t += d
        spans.append(("Ready", t, t))
        if cfg["host_slots"] > 0:
            heapq.heappush(slots, t)
        out[vm] = (cold, submitted, spans)
    return out


def load_cpp_trace(path, from_check):
    actual = {}
    if from_check:
        import io
        for line in io.StringIO(path):
            parts = line.strip().split(",")
            if len(parts) != 7:
                continue
            vm = int(parts[0])
            entry = actual.setdefault(vm, (parts[1] == "1", int(parts[2]), []))
            entry[2].append((parts[3], int(parts[4]), int(parts[5])))
        return actual
    with open(path) as f:
        for row in csv.DictReader(f):
            vm = int(row["vm_id"])
            entry = actual.setdefault(
                vm, (row["cold_image"] == "1", int(row["submitted_ns"]), []))
            entry[2].append((row["phase"], int(row["start_ns"]),
                             int(row["end_ns"])))
    return actual


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    trace_arg, cfg = sys.argv[1], json.loads(sys.argv[2])

    expected = model(cfg)

    # The trace argument is either a CSV file written by `sim --trace` or
    # "-" config-mode: run the C++ check driver with the same config.
    if trace_arg == "-":
        lo_hi = lambda k: f"{cfg[k][0]} {cfg[k][1]}"
        line = " ".join([
            str(cfg["seed"]), str(cfg["num_vms"]), str(cfg["host_slots"]),
            repr(cfg["cold_fraction"]), str(cfg["arrival_gap_ns"]),
            str(cfg["queue_min_ns"]), str(cfg["queue_max_ns"]),
            lo_hi("image_cold"), lo_hi("image_warm"), lo_hi("create"),
            lo_hi("boot"), lo_hi("init"), lo_hi("network")])
        proc = subprocess.run([sys.argv[0].rsplit("/", 2)[0] + "/../build/check"]
                              if False else ["./build/check"],
                              input=line + "\n", capture_output=True,
                              text=True)
        if proc.returncode != 0:
            print("check driver failed:", proc.stderr)
            return 1
        actual = load_cpp_trace(proc.stdout, from_check=True)
    else:
        actual = load_cpp_trace(trace_arg, from_check=False)

    if set(actual) != set(expected):
        print(f"VM set mismatch: cpp {sorted(actual)} vs model "
              f"{sorted(expected)}")
        return 1
    for vm in sorted(expected):
        e_cold, e_sub, e_spans = expected[vm]
        a_cold, a_sub, a_spans = actual[vm]
        if e_cold != a_cold or e_sub != a_sub:
            print(f"VM {vm}: header mismatch cpp cold={a_cold} sub={a_sub} "
                  f"vs model cold={e_cold} sub={e_sub}")
            return 1
        if e_spans != a_spans:
            print(f"VM {vm}: span mismatch")
            print(f"  cpp:   {a_spans}")
            print(f"  model: {e_spans}")
            return 1
    total_spans = sum(len(v[2]) for v in expected.values())
    print(f"AGREE: {len(expected)} VMs, {total_spans} phase spans identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
