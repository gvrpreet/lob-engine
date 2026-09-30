# Experiment: Phase 5(d) ITCH 5.0 converter on a real trading day            Date: 2026-09-28      Commit: 744fc63
**Hypothesis:**
- Replaying the displayed order flow of one Nasdaq stock (adds, executions, cancels, deletes, replaces) rebuilds its book exactly. Evidence would be:
  - zero unknown references and zero engine misses;
  - zero engine trades, because a displayed book never crosses, so any engine trade would mean a mapping error;
  - identical results on every storage variant.
- Real order flow will also settle the open synthetic-data questions (dense vs tree levels, market-maker metrics).

**Change:**
- `replay/itch.hpp` / `replay/src/itch.cpp`: a streaming parser (1 MiB buffer; the file is 12.95 GB) with `convert_itch` and `itch_add_counts`.
- `apps/itch_convert` (`--list` to rank symbols).
- Mapping, as in the header:
  - A/F → New, with the ITCH reference as engine id;
  - E/C/X → Reduce;
  - D → Cancel;
  - U → Modify when the engine would also re-queue (new price or more shares), otherwise Cancel + New. An ITCH replace always loses priority; the engine's same-price reduction would not.
- Tests build ITCH messages byte by byte (`tests/unit/test_itch.cpp`), so no binary fixture is committed. This departs from the plan's "small fixture file"; generating the bytes in code is smaller and easier to read.

**Method:**
- Input: `01302020.NASDAQ_ITCH50` (Nasdaq TotalView-ITCH 5.0, 2020-01-30, 12,952,050,754 bytes, 423,285,709 messages), in git-ignored `data/itch/`.
- Symbols: AAPL, AMD and TQQQ (full day), plus the first 1M events of each, which end mid-session, so the book is non-empty.
- WSL2, Ryzen 7 7840HS, GCC 13.3, Release preset, `taskset -c 2`; timing is the median of 5 `replay_cli --no-strategy` runs.

**Results:**

| Symbol | Symbol messages | Adds / executions / cancels / deletes / replaces | Events out | Unknown refs | Engine trades / misses | Full-day tree ev/s |
|---|---|---|---|---|---|---|
| AAPL (locate 13) | 2,008,973 | 907,157 / 55,395 / 10,161 / 869,314 / 151,325 | 1,993,687 | 0 | 0 / 0 | 15.28M |
| AMD (locate 347) | 2,379,225 | 1,112,128 / 52,350 / 22,262 / 1,071,853 / 108,863 | 2,368,210 | 0 | 0 / 0 | 25.38M |
| TQQQ (locate 7939) | 3,356,107 | 1,539,947 / 27,860 / 195 / 1,519,466 / 266,248 | 3,353,794 | 0 | 0 / 0 | 19.79M |

- Conversion takes about 4.3 s per symbol (one pass over 12.95 GB, 70–136 MB RSS). A `--list` pass takes 9.3 s. Re-conversion is byte-identical.
- Full-day books end empty (0 resting), so the final checksum is only the empty-book hash `14650fb0739d0383`.
- Mid-session cuts (first 1M events) give identical checksums on all 8 variants and both readers:

| Cut | Ends at | Resting orders | Checksum |
|---|---|---|---|
| AAPL | 12:31:57 | 24,628 | `02a0d8b958111323` |
| AMD | 11:52:04 | 11,047 | `4411b56b125fe7c3` |
| TQQQ | 10:40:48 | 3,788 | `404d38369d75b783` |

Level store, full day, M events/s (tree / dense / sorted): AAPL 15.28 / 11.97 / 13.90; AMD 25.38 / 17.83 / 24.80; TQQQ 19.79 / 13.86 / 19.21.

Market maker, full day, latency 0 / 1 µs / 10 µs:
- AAPL: 17,997 / 17,993 / 17,922 fills, 0 aggressive. Marked PnL −39.2M / −39.3M / −39.3M ticks (about −$3,920; 1 tick = $0.0001). Slippage −134.7 ticks/share.
- AMD: 14,104 fills, marked −9.29M ticks at every latency.
- TQQQ: 63,227 fills, marked −44.6M ticks.
- Fill rate is 99.9–100% everywhere.

**Explanation:**
- **Reconstruction holds.** With zero unknown references, zero misses and zero engine trades on 7.7M real events, every add/execute/cancel/delete/replace landed on the right order with the right quantity, and the rebuilt book never crossed.
- **Dense levels are 22–30% slower here, but the comparison is invalid, not a verdict.** `DensePriceLevels` has a band hard-coded to prices 9,000–11,000, the synthetic market's range. AAPL trades near 3,235,250 in ITCH units ($323.53), so every level lives in the overflow `std::map`, and the dense book is the tree plus extra index checks. A fair test needs the band anchored to the instrument, with prices in its real tick: penny-increment stocks only use every 100th ITCH unit, so a ladder in ITCH units would be 99% empty.
- **The market maker's losses are adverse selection, exaggerated by the replay model.** In an ITCH replay, executions are Reduces of the order the feed names. The real aggressor never reaches the engine, so a strategy's resting order is never filled by real taker flow. It is filled only when a displayed Add crosses it, which happens when the market has already moved through the quote. Negative slippage (fills better than the mid at send) combined with large losses is the signature of stale quotes being picked off. The market maker also treats its own order as the touch when it is alone at the best price, so it keeps quoting a price the rest of the market has left. The metrics are correct, but the fill model is one-sided.

**Surprises / next questions:**
1. **Queue-position model.** Strategy fills on ITCH data need one. For example, turn each E/C into an aggressor order against the level, so a strategy order that is ahead in the queue gets filled, then reconcile the named order. Until then, market-maker PnL on ITCH is biased negative and should not be quoted as a strategy result.
2. **Dense band.** Anchor it to the instrument's first price, and add a `tick` option to the converter (divide by 100 for penny stocks, failing if any price isn't a multiple). Then re-run tree vs dense vs sorted on AAPL/AMD/TQQQ. This is the real-data answer Phases 3 and 4 were waiting for.
3. **The market maker should exclude its own order** when reading the touch (it needs a "second-best level" or "volume excluding own" view).
4. **Latency barely matters for AMD** (0 vs 1 µs identical). Its events are mostly more than 1 µs apart, so a 1 µs delay rarely reorders anything. Latency sweeps on real data need 10–100 µs to matter.
