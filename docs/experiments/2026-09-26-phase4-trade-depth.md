# Experiment: Trade-enabled controlled depth            Date: 2026-09-26      Commit: baseline de357e0 + working tree
**Hypothesis:** Add market flow to the deterministic 10/25/50-level streams while keeping anchor orders alive, then compare tree, dense, and sorted stores under actual fills at fixed depth.
**Change:** Extended `generate_depth_events` with an optional market-trade percentage (default 0 preserves the prior add/cancel stream). In trade mode, anchor orders have sufficient quantity to survive the bounded run. `bench_match` accepts a third argument for this rate and reports the configured rate, actual trade count, final levels, and peak levels. The shape pass remains outside timed cases.
**Method:** Ran `taskset -c 2 build/release/bench_match 1000000 <depth> 25` for depths 10, 25, and 50, with three invocations per depth and seven alternating variant runs per invocation. GCC 13.3, Release preset (`RelWithDebInfo`, `LOB_NATIVE=ON`), AMD Ryzen 7 7840HS, Ubuntu 24.04 under WSL2 kernel `6.18.33.2-microsoft-standard-WSL2`; usable hardware counters are unavailable. The generator produces about 200k market trades per run while preserving exact levels.
**Results:** Checksums/trades/depths were stable across the three invocations at each depth: 10 levels `47437d60709689bf`, 200,040 trades; 25 levels `554d243c4f1c0a38`, 200,036 trades; 50 levels `af1805de62b00573`, 200,026 trades.

| Levels | Tree median range | Dense median range | Sorted median range | Aggregate min/max ranges (tree / dense / sorted) |
|---:|---:|---:|---:|---|
| 10 | 25.96-26.47M | 24.83-25.56M | 23.13-23.82M | 21.90-26.99M / 23.18-26.09M / 22.43-24.77M |
| 25 | 22.70-24.01M | 23.71-24.67M | 21.17-21.53M | 21.31-24.59M / 22.15-25.20M / 20.09-22.35M |
| 50 | 21.69-22.72M | 23.62-24.54M | 19.85-20.68M | 19.57-23.23M / 21.84-25.11M / 18.19-21.23M |

Throughput is millions of events/s. Median ranges are across three seven-run invocations; aggregate min/max ranges overlap at each depth. Sorted trailed tree by median at all depths. Dense trailed tree at 10, and led by median at 25/50, but all ranges overlap. Do not change the default based on these samples.
**Explanation:** The controlled stream now exercises market matching while anchors preserve the exact level count. There are no WSL hardware counters to explain the timing. Keep the tree backend as default pending lower-noise native measurements.
**Surprises / next questions:** Repeat on native Linux with branch/cache counters if available; test more than one trade rate and order size. The percentage parameter selects market events after the cancel decision, so record the observed trade count as well as the configured rate.
