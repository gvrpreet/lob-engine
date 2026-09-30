# Experiment: Phase 5(c) Modify and Reduce events            Date: 2026-09-28      Commit: 531e870, 9f1b10c, bfc8e07 (base f38b151)
**Hypothesis:**
- Semantics: adding same-id `Modify` and `Reduce` (partial cancel) under the exchange priority rule keeps the engine identical to the naive oracle on random streams that include both operations. The rule: a same-price quantity reduction keeps queue position; a price change or quantity increase re-queues the order at the back, and it may trade.
- Cost: on v1 logs (New/Cancel only), turning the replay loop's two-way branch into a four-way `switch` changes instructions per event by < ~1%. The two new cases are cold and well predicted, so the 1M checksum `ba7911b3a1dd1d8a` and throughput stay within noise.

**Change:**
- `BasicOrderBook::reduce(id, by)` and `modify(id, price, qty, ts, sink)`, with `ModifyResult` in `lob/types.hpp`. The oracle gets the same in `tests/support/reference_book.hpp`.
- `EventType::{Modify=2, Reduce=3}` under magic `LOBEVT02`. The record layout is unchanged; v1-only logs still write `LOBEVT01`.
- The replay loop dispatches four types and counts `modify_misses` and `reduce_misses`.
- The generator gets an opt-in `modify_percent`; 0 draws no extra random numbers.
- The old behaviour cannot be selected: v1 logs contain no new types, so the regression test is that v1 checksums and instruction counts are unchanged.

**Method:**
- `scripts/check.sh`; differential tests with modify/reduce on 3 seeds and all 8 book variants; golden v1 and a new golden v2 on all variants.
- WSL2: Cachegrind `replay_cli data/events_1000000.bin --no-strategy` I refs/event, base (`f38b151`) vs new; `bench_match 1000000`, 7 alternating pinned runs, base vs new.

**Results:** WSL2, Ryzen 7 7840HS, GCC 13.3. Cachegrind uses a non-native Release build (`-DLOB_NATIVE=OFF`, as `scripts/perf.sh cache` does); timing uses the Release preset (`-march=native`) with `taskset -c 2`.

| Build | I refs (1M v1, `--no-strategy`) | per event | D1 misses | LLd misses | Checksum |
|---|---|---|---|---|---|
| base `f38b151` | 234,919,791 | 234.9 | 4,488,371 | 4,012,998 | `ba7911b3a1dd1d8a` |
| first version (`9f1b10c`) | 279,433,849 | 279.4 (+19.0%) | 4,489,158 | 4,012,914 | same |
| + `always_inline` add/cancel (not committed) | 264,849,370 | 264.8 (+12.7%) | 4,504,140 | 4,012,673 | same |
| + separate TU, but a named sink struct (not committed) | 263,297,330 | 263.3 | 4,503,493 | 4,012,715 | same |
| final (`bfc8e07`): separate TU, local lambda sink | 233,850,376 | 233.9 (−0.5%) | 4,502,082 | 4,012,726 | same |

`bench_match 1000000`, Order 32B, 3 alternating rounds of 7 runs, medians: base 29.17 / 29.71 / 27.23M ev/s; final 29.04 / 26.43 / 29.43M ev/s. Min–max ranges overlap in every round (about 23.4–32.5M).

Modify-enabled stream (`gen_events … 1000000 42 15`: 15% Modify/Reduce): 502,643 trades, 62,786 modify and 31,404 reduce misses, 103,414 resting orders, checksum `571d35b3cfcf1516` on all 8 variants. Median of 5 `replay_cli --no-strategy` runs: 26.61M ev/s, vs 28.51M for the v1 1M log in the same session. These are different workloads, so this is a data point, not a regression figure.

Golden v2 (`golden_v2_2000_seed7.bin`: 1343 New, 339 Cancel, 231 Modify, 87 Reduce): checksum `91319d83ac94152c`, 489 trades, volume 12,124, 98/68/21 cancel/modify/reduce misses, 593 resting. Identical on all 8 variants and both readers.

**Explanation:** The final build's per-function profile matches base: 89.7% of Ir is in `replay_with_book`, with `add`, `cancel`, `std::map::operator[]` and the flat index all inlined. The remaining −0.5% is noise-level code-layout difference, and the miss counts are unchanged. So the prediction (< ~1%) holds for the final structure, but not for the first one. The first version's +45 instructions/event came entirely from lost inlining:
- `add` went out of line with 137.6M Ir, and the two `map::operator[]` with 20.3M + 19.5M.
- The trigger was that `modify`/`reduce` called `add`/`cancel` in the same TU as the loop.
- Forcing `add`/`cancel` inline pushed the flat-index helpers out of line instead.
- A named sink struct gave `add<sink>` external linkage, which disqualifies GCC's called-once inlining. The loop's sink must stay a local lambda.

**Surprises / next questions:** Checksums and timing both said "no change" while the instruction count said +19%. On WSL2, timing noise (±15%) is larger than this regression. Cachegrind I refs are the check to run whenever a change adds call sites or changes template arguments on the hot path. Question for native day: does `perf stat` confirm equal instructions/event for `f38b151` vs `bfc8e07`? The variance runs use the tag, so this would be a separate, optional step.
