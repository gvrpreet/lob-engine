# Experiment: Phase 5(f)+(g) strategy order entry and metrics            Date: 2026-09-28      Commit: 7e6c7ef (base 7e02070)
**Hypothesis:**
- A strategy can trade against the replayed feed through a latency-delayed gateway. Its results (fills, position, PnL, slippage) are deterministic: same log + same strategy + same latency ⇒ bit-identical metrics and book checksum.
- A strategy that sends no orders reproduces `replay()` exactly (same checksum, trades, misses) on golden v1 and v2.
- Building it as a separate replay entry (`replay_trading`, its own TU) leaves the plain `replay()` hot loop unchanged: Cachegrind I refs/event on the v1 1M log stay within noise of `7e02070` (233.9M).

**Change:**
- `replay/include/replay/trading.hpp`:
  - `ITradingStrategy`: `on_event(event, book, gateway)` and `on_fill(fill, gateway)`.
  - `OrderGateway`: `submit`, `cancel`, `modify`. Ids come from the reserved range `kStrategyIdBase = 2^63`.
  - `TradingConfig`: `order_latency_ns`.
  - `StrategyMetrics` and `replay_trading()`.
- `replay/src/trading.cpp`: its own replay loop, a pending-action queue ordered by arrival time, and fill accounting.
- `MarketMaker` in `strategies.hpp`, and `replay_cli --strategy mm [--order-latency ns]`.

**Method:** unit tests with scripted strategies (latency, ids, accounting, PnL, slippage); equivalence of the no-order strategy with `replay()` on both golden files; a pinned market-maker result on golden v1; a determinism re-run; Cachegrind I refs on the v1 1M log, `7e02070` vs new.

**Results:** WSL2, Ryzen 7 7840HS, GCC 13.3, Release preset, `taskset -c 2`, one run per cell.
- `check.sh` passes (78 tests). The scripted-strategy tests pin latency timing, id range, fill flags, position, cash, realised/marked PnL and slippage by hand-computed values. A strategy that sends nothing reproduces `replay()` on golden v1 and v2 (checksum, trades, volume, all miss counts, resting orders).
- Hot loop: plain `replay()` Cachegrind I refs on the v1 1M log, 233,850,376 (`bfc8e07`) → 233,856,560 (+0.003%, CLI code). D1/LLd unchanged; checksum `ba7911b3a1dd1d8a`.
- Market maker (defaults: size 10, max position 200, requote every event). Pinned golden result on `golden_2000_seed7.bin` at 1000 ns: checksum `75d40b3788c04ed5`, 135 orders, 100 modifies, 151 fills (1 aggressive), 1337 of 1357 shares, position +53, cash −529,268. The output is bit-identical across runs and between the mmap and `--read` readers.

| Log | Latency | Fills (aggressive) | Filled / offered | Position | Realised / marked PnL | Avg slippage (ticks/share) | ev/s |
|---|---|---|---|---|---|---|---|
| 1M | 0 | 16,807 (0) | 147,076 / 147,239 | −200 | 43,546 / 43,687 | −0.867 | 25.94M |
| 1M | 1 µs | 16,556 (115) | 144,724 / 144,871 | −204 | 43,278 / 43,410 | −0.871 | 23.47M |
| 1M | 10 µs | 13,852 (1,619) | 122,648 / 122,804 | −208 | 42,541 / 42,711 | −0.909 | 23.59M |
| 5M | 0 | 45,301 (0) | 395,708 / 396,267 | −200 | 126,050 / 127,383 | −1.144 | 16.93M |
| 5M | 1 µs | 44,496 (282) | 388,661 / 389,214 | −201 | 124,916 / 126,241 | −1.153 | 16.65M |
| 5M | 10 µs | 37,352 (4,273) | 329,818 / 330,265 | −206 | 124,104 / 125,442 | −1.233 | 16.35M |

PnL is in ticks × shares. Plain replay of the same logs runs at about 28M (1M) and about 17M (5M) ev/s in this session. The trading loop's extra per-event work (virtual `on_event`, touch lookups) costs roughly 10–15% on 1M and little on 5M, where cache misses dominate.

**Explanation:**
- **Latency behaves as expected.** At 0 ns the market maker only joins the touch, so it never crosses (0 aggressive fills). With latency, a quote can arrive after the touch has moved and cross, so aggressive fills grow with latency (0 → 115 → 1,619 on 1M), and total fills fall because quotes spend more time off the touch.
- **Negative slippage is correct.** Passive fills at the bid or ask are better than the mid by about half the spread.
- **The positive PnL is an artefact of the feed.** The synthetic generator has no informed flow and no adverse selection, so a touch-joining quoter collects spread for free. The PnL figure proves the accounting, not the strategy.
- **The position sits at the limit.** The mid drifts upward over the 5M log (10,000 → 10,365), so the quoter accumulates a short to the limit and stays there.

**Surprises / next questions:**
- The first market-maker draft tracked its own order's remaining quantity. A fill on the old quantity, landing between sending a modify and its arrival, could make that underflow. The fix is `Fill::remaining`, the replayer's exact count, so the strategy never guesses.
- Fill rate (99.9%) is high because an order at the touch in this feed is almost always eventually hit. It will mean more on ITCH data (5d), where queue position and informed flow are real. That is the first workload on which these metrics say anything about a strategy.
- Self-trades are not prevented (orders have no trader identity, as in the Phase 1 note). The market maker can't self-trade, since it never crosses its own quotes at latency 0, but a strategy that did would book both sides.
