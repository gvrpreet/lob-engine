# lob-engine — High-Performance Limit Order Book (C++20)

A price-time priority matching engine (market / limit / cancel) with cache-aware order
storage, plus a deterministic market-replay framework for historical simulation.

This repo is both **a project and a curriculum**: every optimisation phase is paired with
the systems concept it teaches, a test that guards correctness, and a measurement that proves it worked.

```
lob-engine/
├── include/lob/            ENGINE (header-heavy, hot path)
│   ├── types.hpp             ids, ticks, Side/OrderType, Trade, AddResult
│   ├── order.hpp             32-byte resting order (2 per cache line)
│   ├── order_pool.hpp        slab allocator + free list, 32-bit handles
│   ├── price_level.hpp       intrusive FIFO queue per price (O(1) cancel)
│   └── order_book.hpp        matching, cancel, queries, state checksum
├── src/order_book.cpp      cold-path code (checksum)
├── replay/                 REPLAY FRAMEWORK
│   ├── include/replay/       event.hpp (32B record) · event_log.hpp · replayer.hpp
│   │                         synthetic.hpp · strategies.hpp (IStrategy + VwapTracker)
│   │                         latency_histogram.hpp (log buckets) · timer.hpp (steady/rdtscp)
│   └── src/
├── tools/                  gen_events (make a feed) · replay_cli (run + report)
├── bench/                  bench_match (throughput + latency percentiles)
├── tests/                  unit · differential (vs naive oracle) · replay determinism
├── scripts/                build.sh · check.sh · bench.sh · perf.sh · native_{setup,run,compare}.sh
├── docs/
│   ├── 00-roadmap.md         8-phase learning + implementation plan  <-- START HERE
│   ├── 01-architecture.md    data model, invariants, event flow, design decisions
│   ├── 02-workflow.md        daily loop, definition of done, benchmark protocol
│   ├── 03-benchmark-log.md   append-only results table (resume evidence)
│   └── learning/             notes/ (daily log) · experiments/ (hypothesis→result)
├── data/                   generated event logs (git-ignored)
└── CMakeLists.txt · CMakePresets.json · .github/workflows/ci.yml
```

## Quick start
```bash
scripts/build.sh release                       # cmake preset build
ctest --test-dir build/release --output-on-failure
build/release/gen_events data/events_5M.bin 5000000 42
build/release/replay_cli data/events_5M.bin --latency   # add --tsc for the rdtscp timer
scripts/check.sh                               # ASan/UBSan + release tests (run before commit)
```
Requires: CMake ≥ 3.20, GCC ≥ 11 or Clang ≥ 14. Optional: `perf`, `valgrind`, `clang-tidy`.

## Status
Phase 1 unit, differential, and replay checks pass, with added crossing, one-sided-market, partial-fill-cancel, maximum-quantity, and filled-id-reuse cases. A participant self-cross case is not representable without trader identity, and the invariant walkthrough remains unverified. Phase 2(a-c), allocator, record-size, and hot/cold experiments, are implemented and measured; hot/cold did not establish an improvement. Phase 3(a-c) index/level-store swaps are implemented as opt-in variants (tree remains default). Phase 4 has controlled-depth workloads, a log-bucket latency histogram, and an opt-in `rdtscp` timer; cross-day variance checks on a native host remain. See `docs/00-roadmap.md` and `docs/03-benchmark-log.md` for evidence and remaining work.
