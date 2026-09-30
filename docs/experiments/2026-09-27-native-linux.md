# Experiment: native Linux runs (live USB, same laptop)            Date: 2026-09-27      Commit: run 1 `2ca8c98`, run 2 `f609efb` (script-only change)
**Hypothesis:** The questions WSL2 could not answer have measurable answers on native Linux: (1) a hardware counter explains each structural swap; (2) THP cuts dTLB misses on the 5M log; (3) `rdtscp` vs `steady_clock` differ when the host exposes real clock control; (4) TSan runs clean; (5) runs repeat within ~5%. Expected: native throughput at or above WSL2's, with tighter ranges.
**Change:** No engine change. `scripts/native_run.sh full` ran twice on the same day from an Ubuntu 24.04.5 live USB. Between the runs, `f609efb` fixed the script's live-session detection (24.04 ISOs boot `/casper/vmlinuz` without `boot=casper`) and ran the TSan tests under `setarch -R`.
**Method:** AMD Ryzen 7 7840HS (8C/16T, SMT on), 14 GiB, kernel `7.0.0-31-generic`, `amd-pstate-epp`, governor and EPP `performance` on all 16 CPUs, boost on, AC power, clocksource `tsc` (hpet/acpi_pm available), THP `madvise` by default, `perf_event_paranoid=1`, GCC 13.3.0, Release `-O2 -g -DNDEBUG` + `-march=native`, `taskset -c 2` (sibling 3). Runs at 11:18 and 12:26 UTC. The second boot used a persistent live stick. Throughput: `bench_match`, 7 alternating runs per variant. Counters: `perf stat -r 5` on `replay_cli --no-strategy` per variant; these include file load and book setup. THP: system-wide `never` vs `always`, `perf stat -r 5` on the 5M log. Raw output: `results/2026-09-27_111845_full/` and `results/2026-09-27_122639_full/` (git-ignored).
**Results:** All 10 golden checksums matched in both runs, and they match WSL2's.

Structural swaps, 5M standard log (run 1 / run 2). Throughput is the `bench.sh` median in M ev/s.

| Variant | ev/s | IPC | instr/event | branch-miss | cache-miss/event | dTLB-miss/event |
|---|---|---|---|---|---|---|
| Order 32B (tree + flat index, default) | 22.98 / 21.46 | 0.81 / 0.82 | 259 / 263 | 8.13 / 8.10% | 1.81 / 1.81 | 0.230 / 0.302 |
| new/delete | 13.98 / 13.50 | 0.73 / 0.73 | 361 / 363 | 6.02 / 5.93% | 3.05 / 3.04 | 0.638 / 0.548 |
| Order 48B | 21.95 / 20.01 | 0.77 / 0.79 | 267 / 275 | 8.37 / 8.21% | 1.75 / 1.75 | 0.479 / 0.344 |
| Order 64B | 20.23 / 18.93 | 0.71 / 0.73 | 272 / 281 | 8.43 / 8.27% | 1.70 / 1.70 | 0.510 / 0.477 |
| hot/cold | 21.07 / 19.87 | 0.81 / 0.81 | 271 / 272 | 8.16 / 8.10% | 2.19 / 2.22 | 0.267 / 0.282 |
| dense levels | 22.08 / 20.70 | 1.18 / 1.18 | 388 / 391 | 4.03 / 4.04% | 1.80 / 1.83 | 0.334 / 0.281 |
| sorted levels | 21.77 / 20.93 | 0.91 / 0.84 | 305 / 311 | 7.93 / 7.70% | 1.83 / 1.81 | 0.298 / 0.255 |

1M standard log: 32B 32.40 / 32.58; dense 39.10 / 37.91 (IPC 1.33 / 1.35, branch-miss 5.42 / 5.20% vs 32B 8.53%); sorted 31.58 / 29.58 (instr/event 416 / 423 vs 362 / 364). The L1d misses per event for 32B / 48B / 64B were 7.90 / 8.94 / 9.69 (run 1).

Controlled depths, 25% trades, 32B vs dense (run 1 / run 2): 10 levels 34.93 / 31.83 vs 33.08 / 32.14; 25 levels 29.73 / 30.47 vs 34.90 / 35.09; 50 levels 28.08 / 29.43 vs 34.64 / 34.82.

THP, 5M log, `perf stat -r 5` (run 1 / run 2):

| THP | replay ev/s median | dTLB-load-misses | page-faults | elapsed incl. load |
|---|---|---|---|---|
| never | 22.58 / 22.63 | 1,506,194 / 1,430,164 | 55,588 / 55,588 | 0.331 / 0.331 s |
| always | 23.87 / 23.80 | 6,800 / 10,078 | 1,422 / 1,831 | 0.252 / 0.263 s |

Timers: the empty-interval overhead was 20 ns for both `steady_clock` and `rdtscp` in every workload. `bench_match` percentiles agreed within one bucket (5M: 40/131/287/423 vs 50/143/295/423 ns in run 1). `replay_cli --latency` with the VWAP strategy on 5M: steady 10.69M, rdtscp 10.05M ev/s (run 1). In run 2 the steady run was disturbed (7.39M, p99 1311 ns).

TSan: run 1 aborted before any test ran (`FATAL: ThreadSanitizer: unexpected memory mapping`). Run 2, under `setarch -R`, passed the full suite in 80.7 s.

Run-to-run spread (`native_compare.sh`, two same-day runs): 45 of 49 1M workload/variant pairs are within 5%, most within 1%. 5 of 7 5M pairs are above 5% (6.0-9.7%), with run 2 slower in every variant.

**Explanation:**
- **Slab vs new/delete:** explained by two counters. new/delete runs ~100 more instructions per event (allocator calls), has ~1.2 more cache misses per event, and roughly doubles the dTLB misses. This confirms the Phase 2(a) Cachegrind finding with hardware counts.
- **32B → 48B → 64B:** at 5M, cache misses per event stay flat (1.81 → 1.70) while dTLB misses rise (0.23 → 0.48–0.51 in run 1) and IPC falls. The size penalty on the large log looks like TLB reach more than cache capacity. At 1M the L1d misses per event do rise with size.
- **Hot/cold:** no gain, and a small loss at 5M. Cache misses per event are higher (2.19 vs 1.81) and instructions slightly higher. Touching two arrays per order costs more than the smaller hot record saves on this workload. This is consistent with the Phase 2(c) Cachegrind result (more simulated misses).
- **Dense levels:** executes *more* instructions per event (481 vs 362 at 1M) but with half the branch misses and higher IPC (1.33 vs 0.96). Where depth stays bounded (1M standard, 25/50-level controlled) it wins by 14–23%. At 10 levels it only ties. On the 5M log its lead disappears. On the 5M log the gain from fewer branch misses is cancelled by the extra instructions (and a higher dTLB count in run 1). This is the first counter-backed explanation for Phase 3(b).
- **Sorted levels:** more instructions per event (416 vs 362 at 1M) with a similar branch-miss rate, which explains why it trails tree.
- **THP:** strongly explained. dTLB misses fall about 150× and page faults about 35×. `perf stat` counts user plus kernel, so instructions fall from 1.32G to 0.97G: about a quarter of the process's instructions disappeared along with the 4 KB faults. Timed replay (after load) gains 5–6%, and the whole process gains 21–24%.
- **Timers:** the native clocksource is also the TSC, so the WSL2 conclusion holds. rdtscp gives no lower floor, and its `lfence` costs throughput.
- **Variance:** 1M repeatability is good. The 5M shortfall in run 2 comes with deep single-run dips (minimum 12–13M against medians of 20–21M) at the same end-of-run temperature (73 °C), so it looks like intermittent interference rather than throttling.

**Surprises / next questions:**
- Dense levels win on counters that WSL2 never showed (branch misses), not on fewer instructions.
- Hot/cold is measurably worse, not neutral.
- The 64B layout's cost shows up in the TLB, not in LLC misses.
- Candidates for the run-2 dips: writes to the persistent live stick, and khugepaged/compaction after the THP toggle (the disturbed steady-latency step ran right after THP was restored).
- Still open:
  - cross-day variance (two `variance` runs on separate days);
  - counters for the controlled-depth streams (`replay_cli` cannot generate them yet);
  - a flat index vs `unordered_map` counter comparison (the map variant was removed; build `6e1ea00` or re-add it);
  - `MADV_HUGEPAGE` on the pool vs prefault-only (no CLI path calls `prefault_pool()`);
  - timers on a non-TSC clocksource (hpet is available).
