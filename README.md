# lob-engine: a limit order book and market-replay framework (C++20)

[![ci](https://github.com/gvrpreet/lob-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/gvrpreet/lob-engine/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A single-threaded, price-time priority matching engine, and a deterministic replay framework that can drive it with synthetic feeds or with real Nasdaq TotalView-ITCH 5.0 order flow.
- **Matching:** limit and market orders, cancel, same-id modify and partial cancel.
- **Replay:** plain, streaming, timestamp-paced, and a mode where a strategy trades through a latency-delayed order gateway.

It was built as a curriculum, one phase at a time: every optimisation comes with a test that guards correctness and a measurement that explains the result. The design and the measurements behind every number are in [`docs/`](docs/README.md).

## Architecture

```
 Nasdaq ITCH 5.0 ──► itch_convert ──┐                        ┌─► plain replay      (as fast as possible)
 (12.9 GB / day)     one symbol,    ├─► events.bin ─► reader ├─► paced replay      (timestamps → wall time,
 gen_events ────────────────────────┘   LOBEVT01/02  mmap    │                      lateness vs schedule)
 (synthetic feed)                       32 B records read    └─► trading replay    (strategy ⇄ order gateway
                                                     stream                          with simulated latency)
                                                                       │
                     ┌──────────────────────── OrderBook ───────────────▼──────────────────────┐
                     │ bids / asks : dense bitmap band around the touch (re-centres when the    │
                     │               touch leaves it) + overflow std::map for far prices        │
                     │ level       : intrusive FIFO list of 32-bit handles (O(1) cancel)        │
                     │ id index    : open-addressing table, 64-id blocks, Fibonacci-scattered   │
                     │ orders      : 32-byte records in one slab pool (no per-order malloc)     │
                     └─────────────────────────────────────────────────────────────────────────┘
```
Invariants, the event-log format and the design decisions are in [`docs/architecture.md`](docs/architecture.md).

## Results

Unless marked native, the numbers come from WSL2 on a Ryzen 7 7840HS, with GCC 13.3 and `-O2 -march=native`, pinned with `taskset`. The median of 7 runs is shown. Every row in [`docs/benchmark-log.md`](docs/benchmark-log.md) carries its commit, flags and run count.

| What | Result |
|---|---|
| Real order flow, dense levels (default) vs `std::map` levels | AAPL 15.9M vs 13.5M events/s (+18%), AMD +14%, TQQQ +27%, TSLA +27%. Nasdaq 2020-01-30, full day; every dense run beats every tree run. Cachegrind: −46% branch mispredicts |
| Per-event latency, 1M synthetic events (**native** Linux, live USB) | p50 / p99 / p99.9 = 40 / 151 / 263 ns (includes ~20 ns timer overhead) |
| Slab pool vs `new`/`delete` (**native** `perf stat`) | 1.8 vs 3.0 cache misses per event |
| End-to-end, 50M events | 13.2M events/s, 29.3M trades; bit-identical across runs and all three readers |
| Streaming reader | 67–99 MB resident at 5M or 50M events, vs 1.6 GB for mmap/read at 50M (−11–15% throughput) |
| ITCH reconstruction | AAPL, AMD, TQQQ, TSLA rebuilt with 0 unknown references, 0 misses, 0 crossed states |
| Fuzzing | 21.2M operations (dense and tree books vs a naive oracle) clean under ASan/UBSan |

Things the measurements caught along the way (each has rows in the benchmark log):
- A +19% instruction regression hidden by timing noise (lost inlining).
- An order-index hash that collapsed after about 10M events (identity hash plus linear probing).
- A dense-vs-tree conclusion that reversed twice once real data was used.

## Build and reproduce

Requirements: CMake ≥ 3.20 and GCC ≥ 11 or Clang ≥ 14. Clang is needed for fuzzing and clang-tidy. Optional: `valgrind`, `perf`.

```bash
scripts/build.sh release && ctest --test-dir build/release --output-on-failure
scripts/check.sh                                   # ASan/UBSan + Release test suites (the pre-commit gate)
scripts/tidy.sh                                    # clang-tidy, warnings as errors
scripts/fuzz.sh 300 4                              # libFuzzer: 5 minutes, 4 jobs; prints operations checked

# synthetic feed
build/release/gen_events data/events_5000000.bin 5000000 42
build/release/replay_cli data/events_5000000.bin --latency        # throughput, trades, VWAP, p50..p99.9
build/release/replay_cli data/events_5000000.bin --stream         # bounded memory
build/release/replay_cli data/events_5000000.bin --paced 10       # 10x real time, reports lateness
build/release/replay_cli data/events_5000000.bin --strategy mm --order-latency 1000
build/release/bench_match 1000000                                 # all storage variants, 7 alternating runs

# real data: a Nasdaq TotalView-ITCH 5.0 day (public samples: emi.nasdaq.com/ITCH/)
build/release/itch_convert data/itch/01302020.NASDAQ_ITCH50 --list 20
build/release/itch_convert data/itch/01302020.NASDAQ_ITCH50 data/itch/AAPL_c.bin AAPL --tick 100 --drop-off-grid
build/release/replay_cli data/itch/AAPL_c.bin --no-strategy [--tree]

scripts/phase5_exit.sh                             # end-to-end suite, run twice and diffed bit-for-bit
```
Storage variants for comparison (with `--no-strategy`): `--tree`, `--sorted`, `--naive`, `--order-size 48|64`, `--hot-cold`, `--huge-pages`. Native Linux measurements (`perf`, THP, TSan) are scripted in `scripts/native_*.sh`, with an operator manual in [`docs/guides/native-linux-live-usb.md`](docs/guides/native-linux-live-usb.md).

## Repository layout

```
engine/            lob::engine: the order book
  include/lob/       types, 32-byte order, slab pool, price levels (dense/tree/sorted), flat id index, order_book.hpp
  src/
replay/            lob::replay: event format, readers (read / mmap / stream), replay loops (plain / paced / trading),
  include/replay/    ITCH 5.0 converter, synthetic generator, strategies (VWAP, market maker), latency histogram, timers
  src/
apps/              gen_events · replay_cli · itch_convert
bench/             bench_match (all storage variants, alternating runs)
tests/
  unit/              unit, differential (vs a naive oracle), golden-file, replay-mode and ITCH tests
  fuzz/              libFuzzer harness (dense + tree books vs the oracle)
  support/           test harness and the RefBook oracle
  data/              committed golden event logs with pinned results
scripts/           build · check · tidy · fuzz · bench · perf · phase5_exit · stream_measure · native_{setup,run,compare}
cmake/             package config for find_package(lob_engine)
docs/              architecture · workflow · benchmark log · guides/
```

## Using it as a library

```cmake
# as a subdirectory / FetchContent: tests, apps and benchmarks are off unless this is the top-level project
add_subdirectory(lob-engine)
target_link_libraries(my_app PRIVATE lob::replay)   # or lob::engine for the book alone

# or after `cmake --install build/release --prefix <prefix>`
find_package(lob_engine 0.9 REQUIRED)
target_link_libraries(my_app PRIVATE lob::replay)
```
Options: `LOB_BUILD_TESTS`, `LOB_BUILD_APPS`, `LOB_BUILD_BENCH`, `LOB_INSTALL`, `LOB_ASAN`, `LOB_TSAN`, `LOB_NATIVE`, `LOB_FUZZ` (see `CMakePresets.json` for the presets the scripts use).

## Status

Phases 0–5 and 7 are complete; the evidence is in [`docs/benchmark-log.md`](docs/benchmark-log.md). Strategies on ITCH data get queue-position fills: feed executions fill the strategy orders ahead of them in price-time priority. The demo market maker quotes at the others' touch. It is a test strategy, not a profitable one: its day results on real data are near zero and noisy. Still open:
- better strategies (queue-priority preservation, inventory skew) and multi-day data for any statistical claim;
- cross-day variance on a native host;
- Phase 6 (concurrency).

## License

MIT; see [`LICENSE`](LICENSE). Nasdaq TotalView-ITCH sample data is not included; download it from Nasdaq and convert it locally.
