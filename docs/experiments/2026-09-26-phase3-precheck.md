# Experiment: Phase 3 pre-check            Date: 2026-09-26      Commit: a7bb993
**Hypothesis:** The next meaningful throughput gains will come from data-structure changes, not from another storage-layout tweak. A flat open-addressing table for `OrderId -> Handle` and a dense price-level index should cut pointer chasing and branch cost without changing book semantics.
**Change:** Scope the next phase before editing code: keep the 32B AoS default in place, move one data-structure swap at a time, and compare checksum + replay throughput after each swap. Do not change the default memory layout again unless a benchmark shows a clear win.
**Method:** Run the existing differential tests and replay checksum checks after each swap. Measure throughput with the benchmark harness using the same workload and fixed compiler flags from the earlier Phase 2 runs. Keep experiments isolated to one change at a time: first the `std::unordered_map`/`std::map` replacement, then the price-level index strategy.
**Results:** No code path change yet; this is a pre-check to lock scope. The current baseline remains 32B AoS + slab pool + warm-page toggle. The likely comparison points are:
- `std::unordered_map<OrderId, Handle>` vs flat open-addressing table with linear probing and tombstone-free deletion.
- `std::map<Price, PriceLevel>` vs dense array index keyed by `(price - base)` with occupancy bitmap.
- Optional sorted `std::vector` of levels as a secondary comparison for shallow books.
**Explanation:** Phase 2 results showed the cache-layout experiments overlap within noise and did not justify a default change. The remaining likely payoff is structural: fewer pointer indirections, fewer branch mispredicts, and lower per-operation overhead in the hottest lookup paths.
**Surprises / next questions:**
- Which lookup path dominates the replay workload in practice: order lookup by id or price-level traversal?
- Does a dense price index pay off only at deeper books, or are the benefits already visible in the current synthetic replay?
- Should any of the Phase 3 changes be limited to a benchmark-only variant instead of replacing the core engine in-place?
