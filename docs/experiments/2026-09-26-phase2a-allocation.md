# Experiment: per-order allocation vs slab            Date: 2026-09-26      Commit: 12dd168 + working tree
**Hypothesis:** Allocating and freeing each resting order through `std::allocator<Order>` will reduce throughput and increase data-cache misses versus contiguous slab storage due to allocator work and less-local order payloads.
**Change:** `BasicOrderBook<Pool>` shares the matching implementation; `OrderBook` uses `OrderPool` and `NaiveOrderBook` uses `HeapOrderPool`. The latter allocates, constructs, destroys, and deallocates each `Order` through `std::allocator`. `replay_cli --naive` profiles the same log with the baseline policy. Both variants retain handle-based queues, so this does not compare handles with raw pointers.
**Method:** 1M deterministic synthetic events, seed 42 (existing `data/events_1M.bin`, SHA-256 `8676bf28edba0701ebafbdda026f2d09eefb88a3b1b164b213b703ee537046c4`). Timing: Release preset (`-O2 -g -DNDEBUG -march=native`), CPU 2 affinity, one warm-up per policy and seven alternating runs. Cachegrind: same replay via `replay_cli` in a separate non-native Release build (`-O3 -DNDEBUG`), cache simulation enabled.
**Results:**

| Metric | Slab | `std::allocator<Order>` |
|---|---:|---:|
| Throughput median (min–max) | 12.97M (12.54–13.95) events/s | 9.97M (9.40–11.19) events/s |
| Slab speedup | 1.30x | baseline |
| Slab replay latency p50 / p99 / p99.9 | 50 / 560 / 930 ns | not measured |
| Cachegrind D1 misses/event | 6.22 simulated | 7.54 simulated |
| Cachegrind LL data misses/event | 5.00 simulated | 6.25 simulated |
| Cachegrind instruction refs/event | 423.1 | 564.5 |

Both paths produced checksum `ba7911b3a1dd1d8a`. Cachegrind was rerun with strategies disabled for both paths after discovering the first slab profile included VWAP work; the table contains the corrected matched-work counters. Cachegrind totals include CLI parsing, file reading, and book construction as well as replay; normalize by the 1M input events for comparison. These are simulated counts, not hardware counters.
**Explanation:** The direction matches the hypothesis: the slab has about 20% fewer simulated LL data misses and 25% fewer instruction references per event, alongside higher measured throughput. The experiment holds matching, indexes, and handle links constant, so the delta is attributable to allocation/storage behavior plus the heap policy's pointer resolution, not a raw-pointer-vs-handle comparison.
**Surprises / next questions:** WSL2 does not provide a `perf` package for its kernel, and does not expose the CPU governor. Timing spread is notable and should be rechecked on bare-metal Linux before treating the absolute rate as representative. Cachegrind rejected the `-march=native` binary with `Illegal instruction`; a non-native Release build was required. Phase 2(b)-(d) remain unstarted.