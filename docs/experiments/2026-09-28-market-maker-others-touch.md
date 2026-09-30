# Experiment: market maker quotes at the others' touch            Date: 2026-09-28      Commit: 2230665 (base ba4a8b1)
**Hypothesis (from the queue-position note):** the market maker's losses on ITCH data come mostly from reading its own order as the touch. When the real orders at its price leave, it keeps "joining" itself at a price the market has abandoned, and it is filled when the market trades through. If it quotes at the best price held by *others*, those fills should disappear, and so should most of the loss.

**Change:**
- Book: `next_price_after(side, price)`, the next level in priority order, implemented per level store:
  - tree: `upper_bound`;
  - sorted: the neighbouring vector entry;
  - dense: a bitmap scan for the next set bit in the band, merged with the overflow map's `upper_bound`.
- `MarketMaker::Params::exclude_own_orders` (default true): if the best level holds only the market maker's own resting order, it quotes at the next level instead.
- `replay_cli --mm-self-touch` keeps the original behaviour.

**Tests:**
- `next_after` matches the tree on dense (following and pinned bands, queries inside, outside and on the band edges), sorted and tree, for both sides, over random price sets and extreme prices.
- A scripted scenario: the real bid at 100 is cancelled. The market maker steps to the next real level (90), then to 98 when that appears; with `--mm-self-touch` it stays at 100.
- The golden market-maker result is re-pinned: checksum `94793a19cdfa4789`, 140 fills, position +15, cash −149,431. The old behaviour still reproduces its original pin exactly (`75d40b3788c04ed5`, 151 fills, position +53).
- `check.sh` passes (98 tests); plain-replay I refs are unchanged (361.54M).

**Results** (ITCH 2020-01-30, cents, full day, queue-position fills on; PnL in ¢ × shares; "¢/share" = marked PnL ÷ shares filled):

| Symbol | Latency | Self-touch: fills (shares), marked PnL, ¢/share | Others' touch: fills (shares), marked PnL, ¢/share |
|---|---|---|---|
| AAPL | 0 | 28,721 (250,916), −338,601, −1.35 | 5,829 (48,923), **−19,226**, −0.39 |
| AAPL | 10 µs | 27,688 (241,321), −322,715, −1.34 | 6,873 (58,249), **+7,995**, +0.14 |
| AMD | 0 | 22,413 (216,994), −84,837, −0.39 | 9,490 (89,887), **−22,019**, −0.24 |
| AMD | 10 µs | 20,007 (192,814), −71,773, −0.37 | 9,779 (92,596), **−13,929**, −0.15 |
| TQQQ | 0 | 67,630 (669,264), −451,357, −0.67 | 5,243 (50,269), **−96,162**, −1.91 |
| TQQQ | 10 µs | 66,846 (661,573), −444,934, −0.67 | 6,410 (61,869), **−121,603**, −1.97 |
| TSLA | 0 | 27,450 (209,190), −1,729,798, −8.27 | 3,540 (28,602), **+28,112**, +0.98 |
| TSLA | 10 µs | 26,078 (198,259), −1,628,696, −8.21 | 7,533 (57,107), **−261,835**, −4.59 |

Re-quotes rise sharply: AAPL does 364,410 modifies (was 53,371), TQQQ 687,915. With the fix, every fill on these symbols comes from the queue-position model; there are no more fills from Adds crossing a stale quote.

**Explanation:**
- **The hypothesis holds for the bulk of the loss.** Losses fall 4–60× in total, and filled volume falls 3–13×: the removed fills were exactly the stale, picked-off ones. The old market maker's fills were dominated by the market trading through prices it alone still quoted.
- **What remains is not a profitable strategy.** Per share it improves on AAPL and AMD, gets worse on TQQQ, and swings with latency on TSLA (+1 ¢ at 0, −4.6 ¢ at 10 µs). With one day and end positions of ±100–170 shares marked to the close, most of these figures are noise around slightly negative.
- **That is the expected result for a quoter with no signal.** It joins the back of the queue, re-prices constantly (each move loses queue priority, so it is rarely at the front when flow arrives), and is mostly filled when an execution reaches through its level, i.e. when price moves against it. Adverse selection remains; it is no longer the simulator's doing.

**Surprises / next questions:**
- The model and strategy are now in a state where a strategy result can be quoted with its caveats. The next strategy questions are about the strategy, not the simulator: keeping queue priority (not re-pricing on every flicker), inventory skew, quoting only when the spread is wider than a tick, and more days of data for any statistical claim.
- TQQQ is the outlier. A leveraged ETF with a one-tick spread and strong short-term trends would punish a touch-joiner most, and did.
