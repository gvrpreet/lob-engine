# Native Linux checklist and WSL2 reference data

Some measurements cannot be made under WSL2: hardware counters, governor control, the TSan runtime, and cross-day variance on an idle machine. This page lists them, their status, the run order on a native host, and the WSL2 baseline (same CPU) to compare native results against. The operator manual for the live-USB runs is [`native-linux-live-usb.md`](native-linux-live-usb.md). The scripts are `scripts/native_{setup,run,compare}.sh`.

Decision (2026-09-27): run the outstanding measurements on native Linux on the same Ryzen 7 7840HS (a live-USB Ubuntu 24.04), so results sit directly beside the WSL2 rows. Run on AC power, with boost state recorded. A bare-metal cloud instance is the alternative (ordinary VMs often hide PMU counters).

## What needs a native host

Status after native day 1 (2026-09-27). Variance days 2–3 run on git tag `native-variance`, so all three days measure the same code.

| Item | Phase | Why WSL2 can't do it | Native status |
|---|---|---|---|
| Same benchmark on 3 separate days, < ~5% variance | 4 Verify/Exit | No governor control; Hyper-V host noise | Day 1 of 3 done. Same-day sessions: 1M within 5%, 5M 6–10% |
| A counter that explains each structural swap (flat index, dense/sorted levels, 32/48/64B, hot/cold) | 2, 3 Exit | `perf` unavailable for the WSL kernel | Done except the flat index. Slab: instructions + LLC; size: dTLB; hot/cold: more LLC (worse); dense: branch misses; sorted: instructions |
| THP / `MAP_HUGETLB` vs prefault-only | 2(d) | Needs TLB counters | Partly done: system-wide THP always vs never gives dTLB misses 150× lower and replay +5–6%. The pool + index `MADV_HUGEPAGE` variant (`--huge-pages`) is counted on the next native day |
| `rdtscp` vs `steady_clock` overhead | 4 | WSL2 clocksource is already `tsc` | Done: 20 ns each; the native clocksource is also `tsc` |
| TSan runtime | 0, 6 | Runtime unsupported under WSL2 | Done: passes under `setarch -R` |

## Run order on the native host
1. Setup: `kernel.perf_event_paranoid=1`, governor `performance`, and check `/sys/devices/system/clocksource/clocksource0/current_clocksource`.
2. `scripts/check.sh`, plus `scripts/build.sh tsan` and its tests.
3. `perf stat` per variant on the 1M and 5M logs, and on the controlled 10/25/50-level streams with trade rate 25.
4. `bench_match` timer overhead: `steady_clock` vs `rdtscp`.
5. THP (`MADV_HUGEPAGE`) vs prefault-only, with dTLB miss counters.
6. `scripts/bench.sh 5000000 2` on three separate days, for the variance check.
7. Append every result to `../benchmark-log.md` with hardware, compiler, flags, commit and run count.

`scripts/native_run.sh` automates steps 1–6 with full logging (`full` for day 1, `variance` for days 2–3). The repo reaches the live system as a `git bundle`, and `data/*.bin` are regenerated and checksum-verified by the run script.

## WSL2 reference data (same CPU)

These were recorded when the `std::map` tree was still the default level store (before 2026-09-28).

**Environment (2026-09-27, commit `4ba8e38`):**
- AMD Ryzen 7 7840HS (8 cores / 16 threads, SMT on), 7 GiB RAM visible to WSL2, kernel `6.18.33.2-microsoft-standard-WSL2`.
- GCC 13.3.0, Release preset `-O2 -g -DNDEBUG` + `-march=native` + `-fno-omit-frame-pointer`, `taskset -c 2`.
- Clocksource `tsc`; CPUID invariant TSC = yes; no cpufreq governor exposed; THP `madvise`; `perf` unavailable.
- Each cell is 7 alternating runs in one `bench_match` invocation (median, min–max, M events/s).

**Throughput** (`bench_match <events> [levels trade%]`):

| Variant | 1M standard | 5M standard | 1M, 10 levels, 25% | 1M, 25 levels, 25% | 1M, 50 levels, 25% |
|---|---|---|---|---|---|
| Order 32B (then-default: tree + flat index) | 31.86 (28.69–33.99) | 16.20 (14.43–19.34) | 30.26 (26.78–31.96) | 25.88 (22.17–28.76) | 24.97 (21.97–26.85) |
| new/delete | 19.83 (17.64–22.58) | 10.01 (8.96–11.65) | 17.02 (12.86–19.00) | 13.49 (10.00–15.47) | 14.55 (13.52–15.45) |
| Order 48B | 30.03 (26.73–32.47) | 18.12 (16.88–18.99) | 27.25 (25.09–31.00) | 23.20 (17.17–27.09) | 22.51 (21.17–24.17) |
| Order 64B | 26.75 (24.55–30.77) | 16.37 (15.39–17.70) | 26.42 (21.68–27.45) | 21.10 (20.04–24.61) | 21.64 (20.50–24.91) |
| hot/cold | 28.64 (27.43–31.41) | 16.21 (13.34–17.53) | 31.60 (25.97–33.48) | 25.61 (23.20–27.57) | 24.38 (20.27–26.43) |
| dense levels | 34.24 (29.02–37.18) | 16.93 (12.15–18.35) | 26.98 (24.85–33.84) | 27.37 (22.41–30.64) | 28.86 (22.87–30.32) |
| sorted levels | 28.44 (25.24–31.79) | 16.91 (13.39–18.92) | 27.92 (22.38–31.48) | 22.93 (21.00–27.20) | 22.61 (21.30–23.57) |
| Checksum | `ba7911b3a1dd1d8a` | `01e9c2940a0e996f` | `47437d60709689bf` | `554d243c4f1c0a38` | `af1805de62b00573` |
| Trades / final / peak levels | 528,626 / 133 / 135 | 3,406,476 / 170 / 223 | 200,040 / 10 / 10 | 200,036 / 25 / 25 | 200,026 / 50 / 50 |

**Latency** (then-default book, ns, p50 / p90 / p99 / p99.9, timer overhead ~19 ns included):

| Workload | steady_clock | rdtscp |
|---|---|---|
| 1M standard | 38 / 97 / 239 / 407 | 38 / 125 / 279 / 455 |
| 5M standard | 39 / 167 / 343 / 559 | 48 / 187 / 367 / 591 |
| 1M, 10 levels, 25% | 29 / 77 / 251 / 391 | 28 / 67 / 243 / 375 |
| 1M, 25 levels, 25% | 29 / 211 / 335 / 471 | 37 / 171 / 319 / 455 |
| 1M, 50 levels, 25% | 39 / 139 / 303 / 471 | 39 / 139 / 295 / 471 |

`replay_cli data/events_5000000.bin --latency` (VWAP strategy on): 9.72M ev/s; p50/p90/p99/p99.9 = 39/179/351/559 ns; 740,100 cancel misses; 298,964 resting at end. TSC calibration varied 3.894–4.010 GHz across five invocations (about 3%).

**Cross-session variance on WSL2** (the Phase 4 Verify target is < ~5%): the same 1M seed-42 log gave medians of 29.70–30.91M, 22.62–23.06M and 31.86M in three sessions. That is roughly 40% between sessions, versus 1–5% within a session.

**Counter substitutes on WSL2** (Cachegrind, simulated; native `perf` replaces these):

| Comparison | D1 misses/event | LLd misses/event | I refs/event | Source |
|---|---|---|---|---|
| slab vs `std::allocator` | 6.22 vs 7.54 | 5.00 vs 6.25 | — | Phase 2(a) |
| 32B / 48B / 64B | 6.22 / 6.66 / 7.10 | 5.00 / 5.52 / 6.00 | — | Phase 2(b) |
| AoS vs hot/cold | 6.21 vs 6.38 | 5.00 vs 5.04 | 430.5 vs 425.0 | Phase 2(c) |
| `unordered_map` vs flat index | 20.1 vs 20.6 | 17.6 vs 19.4 (flat higher) | 773 vs 490 | Phase 3(a), 100k-event harness |
| latency off / vector+steady / histogram+steady | 4.49 / 5.57 / 4.51 | 4.01 / 4.96 / 4.01 | 254.9 / 380.3 / 352.9 | Phase 4 tsc-histogram |

Each row uses its own build and run options (see the Source column). Compare within a row, not across rows.
