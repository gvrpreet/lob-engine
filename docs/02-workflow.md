# Workflow

## The loop (per change)
```
 1. HYPOTHESIS   write it in docs/learning/experiments/YYYY-MM-DD-name.md
                 ("Replacing unordered_map with flat table cuts LLC misses/event by ~X because ...")
 2. TEST FIRST   add/extend a test (unit or differential) that pins behaviour
 3. IMPLEMENT    smallest change; keep the old implementation selectable until proven
 4. CHECK        scripts/check.sh           (ASan+UBSan + release tests)
 5. MEASURE      scripts/bench.sh           (pinned, warm, multi-run)  +  scripts/perf.sh stat
 6. EXPLAIN      does a counter explain the delta?  (instructions? IPC? cache/branch misses?)
 7. RECORD       append a row to docs/03-benchmark-log.md, finish the experiment note
 8. COMMIT       one logical change per commit:  "phase3: flat id index (+18% ev/s, -30% LLC miss)"
```
If step 6 has no explanation, you don't understand the change yet — that's the actual learning signal.

## Definition of done
- [ ] `scripts/check.sh` passes (ASan/UBSan + release)
- [ ] Differential test passes with ≥ 3 seeds; replay checksum unchanged vs previous commit
- [ ] Benchmark entry added (hardware, compiler, flags, commit, N runs)
- [ ] Invariants in `01-architecture.md` still hold (`validate()` once it exists)
- [ ] Experiment note filled in (hypothesis → result → what surprised you)

## Daily / weekly cadence
- **Daily (60–120 min):** 25 min read/watch → 60 min build → 10 min notes (`docs/learning/notes/TEMPLATE.md`).
- **Weekly:** phase review — check exit criteria, update the resume evidence map, decide next experiment.
- **Rule:** re-implement, don't copy. For each phase, write the code from scratch first, then compare with the reference.

## Branching & commits
`main` always green. Branches: `phaseN/<topic>`. Tag milestone commits `phase1-baseline`, `phase3-flat-index` so you
can benchmark any historical state: `git checkout phase1-baseline && scripts/bench.sh`.

## Benchmark protocol (numbers you can defend)
1. Build `release` preset (`-O2/-O3`, debug symbols, frame pointers). `-march=native` only for local perf work; note it.
2. Set CPU governor to `performance`; close browsers/IDEs; ideally disable turbo for stability, or record that it's on.
3. Pin: `taskset -c <core>` (prefer a physical core not shared with SMT sibling; `isolcpus` for the best isolation).
4. Generate the log **outside** the timed region (done in `bench_match`); warm-up run first (page-faults the pool, warms caches).
For controlled depths, run `build/release/bench_match 1000000 10`, then repeat with `25` and `50`. To add market flow, pass a third argument such as `25` (configured percent); record actual trades and final/peak depth. The trade-enabled generator preserves anchor levels.
`bench_match` also reports final and peak active price-level counts from a separate untimed pass; include these in interpretation so structural comparisons are tied to actual book depth.
5. ≥ 7 runs; report min / median / max, not just best. Big spread ⇒ noisy machine, fix that first.
6. Report throughput **and** latency percentiles (p50/p90/p99/p99.9). Never report the mean latency alone.
7. Know your timer: `steady_clock` costs ~20–30 ns/call, so per-event latency has a floor. `bench_match` and `replay_cli --latency` print the median empty-interval overhead, so record it with every percentile. `replay_cli --tsc` selects the calibrated `rdtscp` timer. Where the kernel clocksource is already `tsc` (check `/sys/devices/system/clocksource/clocksource0/current_clocksource`), it is no cheaper, and its `lfence` costs throughput. For sub-overhead resolution, time batches.
8. Record: CPU model, cores/SMT, RAM speed, kernel, compiler + version, flags, commit hash, event count, seed.
9. Check the checksum is identical across runs/commits so you know you measured the same work.
10. Be aware of workload realism: the synthetic feed's book depth and cancel ratio strongly affect results. Say which workload you used. Later, validate against ITCH-derived data.

## Profiling recipes
```bash
scripts/perf.sh stat                    # IPC, branch-misses, cache-misses: "is it memory- or branch-bound?"
scripts/perf.sh record                  # where do cycles go? (uses frame pointers)
scripts/perf.sh cache                   # cachegrind: deterministic D1/LL miss counts by source line
perf stat -e dTLB-load-misses ...       # TLB pressure ⇒ try huge pages
```
Reading the result: low IPC + high LLC misses ⇒ layout/pointer chasing (Phase 2–3). High branch-misses ⇒ unpredictable
compares in match/hash probe. High instructions/event ⇒ algorithmic work (e.g. `std::map` rebalancing).

## Debugging checklist when a differential test fails
1. Print the seed and first diverging op index (add to the test).
2. Shrink: bisect the op count until the smallest failing prefix; turn it into a permanent unit test.
3. Run ASan/UBSan; then `validate()` after every op.
4. Compare `checksum()` of book vs oracle state at the failing step.
