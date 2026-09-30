# Experiment: hot/cold order storage            Date: 2026-09-26      Commit: 12dd168 + working tree
**Hypothesis:** Keeping quantity and FIFO links in a compact hot array, while moving id, price, and side to separate cold storage, may improve matching locality. It may regress when fills/cancels frequently need the cold fields or when the split increases total footprint.
**Change:** Added `HotColdOrderPool` with a 12B hot record (`qty`, `prev`, `next`) and a 24B cold record (`id`, `price`, `side`). The matching book and price-level queue now use pool accessors, allowing AoS and split storage to share logic. `replay_hotcold`, `replay_cli --hot-cold --no-strategy`, and the rotating benchmark include this policy. The default remains AoS `Order` (32B).
**Method:** 1M deterministic synthetic events, seed 42, checksum `ba7911b3a1dd1d8a`. Timing used Release preset (`-O2 -g -DNDEBUG -march=native`), CPU 2 affinity, warm-up, seven rotating runs across five policies. Cachegrind used the same event file, strategies disabled for both, and a separate non-native Release build (`-O3 -DNDEBUG -std=c++20`).
**Results:**

| Metric | AoS 32B | Hot/cold |
|---|---:|---:|
| Throughput median (min–max) | 14.89M (13.12–19.04) events/s | 15.13M (11.42–17.69) events/s |
| Replay latency p50 / p99 / p99.9 | 50 / 381 / 712 ns | not measured |
| Cachegrind D1 misses/event | 6.21 simulated | 6.38 simulated |
| Cachegrind LL data misses/event | 5.00 simulated | 5.04 simulated |
| Cachegrind instruction references/event | 430.5 | 425.0 |

**Explanation:** The hot/cold median is 1.6% higher, but the wide overlapping timing ranges do not show a reliable throughput win. Cachegrind simulates slightly more data misses and marginally fewer instruction references. Keep the 32B AoS default; this experiment does not justify a storage change.
**Surprises / next questions:** Splitting fields reduced simulated instruction references slightly but did not improve simulated misses. The WSL2 environment still lacks hardware `perf` counters and has noisy timing. Phase 2(d) pool pre-faulting and huge-page behavior is the next roadmap item.