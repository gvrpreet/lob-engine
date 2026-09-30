# Experiment: queue-position fill model for strategies on ITCH data            Date: 2026-09-28      Commit: eb80a7f (base 5239ee6)
**Problem (from 5d):** in an ITCH replay, a feed execution only reduces the order it names; the real aggressor never reaches the engine. A strategy's resting order was therefore filled only when a displayed Add crossed it, i.e. after the market had already moved through it. Fills were one-sided and adverse.

**Model (user choice: b, queue-ahead):** keep the feed book exact, and let feed executions also fill the strategy orders that price-time priority puts ahead of the executed order.
- On an ITCH execution (E/C) of feed order *o* at price *p* on side *s*, the unseen aggressor would have hit first:
  - every strategy order on side *s* at a price better than *p*;
  - the strategy orders at *p* queued ahead of *o* in the engine's FIFO list (the engine already keeps arrival order).
- Those are filled as makers at their own price, best price first and then time priority, up to the executed quantity. *o* is still reduced by the full executed amount, so the book stays exact. The strategy is assumed too small to change what the aggressor did.
- An execution of an order *ahead* of the strategy order only moves it up the queue, which the engine already does by reducing that order.

**Change:**
- Event format v3 (`LOBEVT03`) adds `EventType::Execute = 4`. Every replay loop treats it exactly like `Reduce`, so book state and checksums are unchanged. `write_events` writes the lowest version that fits, so v1/v2 files stay byte-identical.
- The ITCH converter emits Execute for E/C (X stays Reduce).
- The engine gets two read-only queries for the model: `find_order` and `for_each_order_at`.
- `TradingConfig::queue_fills` is on by default; `replay_cli --no-queue-fills` restores the one-sided behaviour for comparison. `StrategyMetrics::queue_fills` / `queue_fill_qty` count the fills the model produced.

**Tests:** `check.sh` passes (96 tests). A scripted scenario pins the model:
- an execution ahead of the strategy bid gives no fill;
- an execution of a later order at the same price fills 3;
- an execution at a worse price fills the remaining 2;
- the feed book stays exact;
- with the model off, there are no fills at all.

Other tests: the file-version writer/reader, and the ITCH mapping of E/C to Execute and X to Reduce. The re-converted v3 logs give unchanged mid-session checksums (AAPL `c9917ec94f85c69d`, AMD `3af832aa3a10512c`, TQQQ `3299659deadc2495`), and the hot loop stays at 361.54M I refs.

**Results** (market maker with defaults: size 10, max position 200; full day, cents; latency 0; PnL in cents × shares):

| Symbol | Fills (shares), one-sided | Fills (shares), queue model | of which queue fills | Marked PnL, one-sided → queue | Position at end |
|---|---|---|---|---|---|
| AAPL | 17,997 (168,241) | 28,721 (250,916) | 17,794 (147,744) | −391,814 → −338,601 | −41 → +154 |
| AMD | 14,104 (140,585) | 22,413 (216,994) | 11,368 (107,472) | −92,860 → −84,837 | −155 → +164 |
| TQQQ | 63,227 (629,104) | 67,630 (669,264) | 8,974 (86,251) | −445,954 → −451,357 | −120 → −70 |
| TSLA | 11,849 (103,637) | 27,450 (209,190) | 21,348 (157,858) | −1,737,722 → −1,729,798 | −109 → +12 |

At 10 µs order latency the picture is the same (AAPL: 27,688 fills, −322,715).

**Explanation:**
- **Fills become two-sided.** With the one-sided model the end positions sat near the short limit on three of four symbols. With the queue model the fills come from both sides. Fill volume rises 1.5–2× (AAPL, AMD, TSLA). TQQQ gains least (+7%): its market maker was already filled mostly by crossing adds, so its quotes rarely sat ahead of executions.
- **The market maker still loses: about 1.35 ¢ per share on AAPL.** The fill model is no longer the obvious cause; the strategy is. It re-quotes at "the touch" as the book reports it, which includes its own order. When the real orders at its price leave, it stays alone at a stale price, and executions at worse prices then fill it through rule 1 (the market moved through it). That is real adverse selection, and it is a strategy flaw (follow-up 3), not a simulator artefact.

**Surprises / next questions:**
- **Model limits.**
  - The aggressor's full size is unknown: the strategy's fills are capped at each execution message's quantity.
  - Hidden and reserve liquidity is invisible.
  - The strategy's own fills don't take liquidity away from the feed. That is fine for small size, wrong for large.
- **Next:** make the market maker exclude its own order when it reads the touch (follow-up 3). Then the PnL of a *sensible* quoter becomes the first strategy result worth quoting.
