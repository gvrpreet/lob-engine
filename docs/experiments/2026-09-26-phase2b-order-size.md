# Experiment: resting-order size variants            Date: 2026-09-26      Commit: 12dd168 + working tree
**Hypothesis:** Increasing each resting order from 32B to 48B or 64B increases pool footprint and cache misses; any throughput change should be judged against run-to-run spread.
**Change:** Added 48B and 64B padded order records. The 64B record is aligned to a cache line. Templated slab storage and replay allow 32B, 48B, and 64B variants to use the same matching, price-level, and index code. The 32B record remains the default.
**Method:** 1M deterministic synthetic events, seed 42; one warm-up per size, seven alternating runs, timing Release preset (`-O2 -g -DNDEBUG -march=native`) pinned to CPU 2. Cachegrind used the same saved event file with strategy callbacks disabled and a separate non-native Release build (`-O3 -DNDEBUG -std=c++20`).
**Results:**

| Metric | 32B | 48B | 64B |
|---|---:|---:|---:|
| Throughput median (min–max) | 16.71M (11.90–18.42) events/s | 17.08M (13.36–18.78) events/s | 16.44M (14.20–18.13) events/s |
| Replay latency p50 / p99 / p99.9 | 50 / 431 / 732 ns | not measured | not measured |
| Cachegrind D1 misses/event | 6.22 simulated | 6.66 simulated | 7.10 simulated |
| Cachegrind LL data misses/event | 5.00 simulated | 5.52 simulated | 6.00 simulated |
| Cachegrind instruction references/event | 430.2 | 427.8 | 427.9 |

All layouts produced checksum `ba7911b3a1dd1d8a`. The 48B median is only 2.2% above 32B, smaller than the observed timing spread; no throughput winner is established. The cache simulation shows larger records causing more D1 and last-level data misses for this workload.
**Explanation:** The larger records reduce the number of orders per cache line and enlarge the fixed-capacity pool. That is consistent with increasing simulated data misses. Differences in simulated instruction references are small and do not explain the noisy native timing results. Keep 32B as the default until a lower-noise hardware measurement supports a change.
**Surprises / next questions:** 48B was fractionally faster by median in this run, but the ranges overlap widely. WSL2 still lacks usable `perf` counters and an exposed governor. Phase 2(c) hot/cold split is the next build task; Phase 2(d) pool pre-fault/huge-page experiments remain outstanding.