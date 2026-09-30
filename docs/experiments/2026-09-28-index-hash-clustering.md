# Experiment: flat order-index hash (identity clustering collapse)            Date: 2026-09-28      Commit: b47f506 (base cfbb255)
**Found by:** the Phase 5(b) 50M-event measurement. One `replay_cli` run had not finished after 4 minutes; at the 5M rate it should take about 3 s.

**Hypothesis:** `FlatOrderIndex` hashed ids with `std::hash<OrderId>`, and libstdc++ implements that as the identity. Order ids are sequential, and the table uses linear probing with 2× headroom (2^21 slots for 2^20 orders). Once ids wrap the table several times while old orders keep resting, each new sequential id lands just behind survivors and pushes forward into the next id's home slot. The run "surfs" along and grows into huge primary clusters. So the cost per lookup should grow with log length, not with how full the table is.

**Confirmation** (same generator, prefix-stable, default book, `replay_cli --no-strategy`):

| Events | Resting at end | ev/s |
|---|---|---|
| 5M | 298,964 | 18.4M |
| 10M | 604,792 | 15.5M |
| 15M | 772,853 (37% of slots) | **0.41M** |
| 20M | — | did not finish in 150 s |

The table is only 37% full at 15M, so this is clustering, not load.

**Change:** the home slot is now computed from the id rather than equal to it. The rule: blocks of 2^b consecutive ids stay contiguous, and the blocks are scattered with Fibonacci hashing (`(id >> b) × 2^64/φ`, top bits). `b` is the compile-time knob `LOB_INDEX_BLOCK_BITS`, default 6 (64 ids = 1 KB of entries). Checksums are unchanged on every log (the index affects storage only). The wrapped-cluster deletion test now finds colliding ids under the real hash, and a churn test covers sequential ids over many wraps.

**Results** (WSL2, Release preset, `taskset -c 2`; median of 5 alternating rounds, M ev/s):

| Hash | 1M | 5M | 10M | AAPL cents | 15M | 20M | 50M (1 run) |
|---|---|---|---|---|---|---|---|
| identity (old) | **28.06** | **19.78** | 14.78 | **16.96** | collapse | collapse | collapse |
| Fibonacci per id (b=0) | 15.72 | 14.46 | 13.74 | 15.91 | 13.30 | 12.56 | 13.16 |
| b=4 (16 ids) | 22.66 | 16.65 | 15.06 | 15.48 | 14.03 | 13.02 | — |
| **b=6 (64 ids), chosen** | 23.29 | 17.39 | 15.58 | 15.95 | 14.09 | 12.95 | **13.60** |
| b=8 (256 ids) | 26.74 | 18.67 | 15.91 | 16.14 | 13.19 | 11.34 | 10.29 |
| b=10 (1024 ids) | 27.26 | 19.02 | 15.43 | 16.09 | 11.36 | 8.27 | — |

Cachegrind (non-native Release), per event, identity → b=6:
- v1 1M: I 233.9 → 259.7, D1 4.50 → 4.52, LLd 4.01 → 4.03.
- AAPL cents: I 385.6 → 412.2, D1 3.22 → 3.23, LLd 2.36 → 2.36.
- Per-function Ir: everything is still inlined into `replay_with_book`.
- Scattering every id (b=0) instead raised v1 1M D1 to 5.12 and LLd to 4.70 per event.

**Explanation:**
- **Why identity was fast:** locality. The table is 32 MB (twice the L3), and identity keeps each new id right next to the previous one, so inserts and `contains` checks hit warm lines and the prefetcher.
- **Why b=0 is slow on small logs:** scattering every id throws that away. Every new id is a likely LLC miss, which gives the higher D1/LLd per event and the −43% at 1M.
- **What b=6 does:** 64-id blocks keep the locality (misses equal to identity) while the scattering between blocks stops runs from surfing across the table.
- **Where the remaining cost comes from:** instructions. The multiply-and-shift home slot is computed on every probe, and on every step of `erase`'s backward-shift scan, where identity cost nothing. That is the −17% at 1M and −12% at 5M against identity.
- **Why bigger blocks lose on long logs:** they bring the clustering back gradually. b=8 is already slower at 20M and 50M, and b=10 at 15M+.

**Surprises / next questions:**
- Every benchmark before this used at most 5M events, so a performance-correctness bug in the default book was invisible for the whole of Phases 3 and 4. Benchmarks now need at least one run far beyond the table size.
- The 50M synthetic log saturates the 1M-order pool: 1,048,576 resting at the end and 8.36M rejected adds, because the generator leaks far-away orders. The 50M figures therefore measure a saturated book.
- Ways to recover the small-log instructions: cache the home slot next to the entry (costs space), or use Robin Hood hashing with a stored probe distance, so that deletion needs no rehash.
- ITCH order references are also sequential, so a full-day busy symbol could have hit this. The three converted symbols stayed below about 1.5M live orders and never wrapped far enough.
