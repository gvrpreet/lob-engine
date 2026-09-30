# Experiment: re-centring dense band, then dense levels as the default            Date: 2026-09-28      Commits: 8ac27d2 (+ 3ee2175) (base fe04bde)
**Hypothesis:**
- The anchored band (centred on the first price) fails on days when the touch drifts more than about span ÷ 2 (±$10 in cents) from the pre-market price; the touch then runs through the overflow `std::map`.
- Re-centring whenever the touch leaves the band should cost next to nothing where the anchor already fits, and win where it doesn't.
- If that holds, dense becomes the default (user decision of 2026-09-28).

**Change:**
- `DensePriceLevels` (default-constructed) re-centres. When a new level outside the band becomes this side's best price, every level (band and overflow) is moved into a band centred on it. Levels move by value: orders refer to levels by price, and levels are only created after matching, so no pointer survives the move.
- Pinned bands (explicit `base`) never move. `recentres()` counts the rebuilds.
- `BasicOrderBook`'s default level store is now `DensePriceLevels`, so every storage variant moves with it (single-factor comparisons stay valid). `TreeOrderBook`, `replay_tree` and `replay_cli --tree` keep the `std::map` store. `--dense` now errors with a hint, `bench_match`'s "dense levels" row is now "tree levels", and `native_run.sh`'s perf variant `dense` is now `tree`.
- Hot-loop fix: the outside-band path moved into a `noinline` function, and `recentre` is `noinline, cold`.

**Tests:**
- A unit test for re-centring and pinning.
- A differential test in a drifting market (a random walk of about ±6,000 ticks over 60k ops): dense and sorted against tree, checking results, trades and checksums.
- A mutation test, a rebuild that forgets the overflow levels, produced more than 24,000 failures.
- The old dense-vs-tree tests now compare against `TreeOrderBook` explicitly, and `tree_differential_seed_1` keeps the random differential on the tree store. `check.sh` passes (93 tests).

**Results** (WSL2, Release preset, `taskset -c 2`, `--no-strategy`; M ev/s, median of 7 alternating rounds, min–max):

Re-centring vs anchored, both as the `--dense` variant (before the default switch):

| Log | Tree | Dense anchored | Dense re-centring |
|---|---|---|---|
| AAPL cents | 14.64 | 17.45 | 17.31 |
| AMD cents | 21.63 | 24.07 | 23.81 |
| TQQQ cents | 17.63 | 21.21 | 21.13 |
| **TSLA cents** (2.0% of prices within ±$10 of the first) | 12.38 (11.54–12.76) | 12.15 | **15.10 (14.17–16.62)** |
| synthetic 1M | 26.66 | 24.77 | 28.19 |
| synthetic 5M | 16.38 | 15.37 | 15.52 |

Cachegrind, TSLA, per event (tree / anchored / re-centring): I 511.7 / 520.7 / 446.1; mispredicts 8.73 / 6.59 / 4.27. AAPL: I 412.2 / 354.6 / 364.6; mispredicts 5.61 / 3.41 / 3.33.

Dense as the default (after the switch and the hot-loop fix), default vs `--tree`:

| Log | Default (dense) | `--tree` |
|---|---|---|
| AAPL cents | **15.90 (14.61–16.44)** | 13.45 (12.47–13.66), +18% |
| AMD cents | **23.34 (21.27–24.59)** | 20.42 (18.79–20.62), +14% |
| TQQQ cents | **20.43 (19.81–20.62)** | 16.07 (15.54–16.46), +27% |
| TSLA cents | **14.35 (13.19–14.70)** | 11.34 (9.69–11.56), +27% |
| synthetic 1M | 22.86 (21.84–24.35) | 22.60 (14.29–23.98), +1%, overlap |
| synthetic 5M | 13.77 (13.41–14.34) | 14.60 (12.93–14.85), −6%, overlap |

Cachegrind with dense as the default, per event, default vs tree:
- v1 1M: I 361.5 vs 252.4, mispredicts 2.78 vs 3.52.
- AAPL: I 361.3 vs 410.5, mispredicts 3.12 vs 5.76.

Checksums are unchanged on every log: 1M `ba7911b3a1dd1d8a`, 5M `01e9c2940a0e996f`, AAPL cut `c9917ec94f85c69d`, and all golden files.

**Explanation:**
- **Re-centring works where it should.** On AAPL, AMD and TQQQ the market stayed within ±$10 of the pre-market price, so re-centring changes nothing (−1%; the extra "becomes best?" check on the overflow path is about +10 instructions/event). On TSLA, the day after its earnings, only 2% of prices were near the first one. There the anchored band was no better than the tree, while re-centring gives −13% instructions, −51% mispredicts and +22%.
- **The default switch first broke inlining.** As the default, the dense book also compiles the strategy callback into `replay_with_book<OrderBook>`, and with the rebuild inlined into `get_or_create`, GCC pushed the flat index and `get_or_create` out of the loop: 376.4 vs 274.1 I/event against tree, and 5M −9%. Moving the outside-band path out of line brought it to 361.5. `FlatOrderIndex::insert` and `get_or_create` still show as separate functions, but real-data timing improved on every symbol.
- **Real data: dense wins by 14–27%, with no overlap.** Fewer mispredicts (−46% on AAPL) and fewer instructions (−12%).
- **Synthetic data: dense executes more instructions**, because every best-price lookup scans the bitmap and this generator's book is shallow near the touch. That costs up to 6% at 5M, within noise. Since real data is the representative workload (the Phase 5 start decision), dense is the default.

**Surprises / next questions:**
- The 5(c) lesson applied again: changing *which type is the default* changes what gets compiled into the hot loop. The new hot-loop baseline is 361.5 I/event (v1 1M, default book).
- Recovering the rest: `insert` is out of line (18.7M Ir on 1M). A summary bitmap (a 2-level index) would make best-price scans O(1) and cut synthetic instructions. Caching the best index per side is the other option.
- Sorted levels lose on real data (the book is too wide) and stay experimental.
