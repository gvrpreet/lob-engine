# Architecture & schema

## 1. Data model
```
OrderId  u64        Price  i64 (ticks)     Qty  u32      Handle u32 (pool index)

Order (32 B)  ─ id:u64 │ price:i64 │ qty:u32 │ prev:u32 │ next:u32 │ side:u8 │ pad
PriceLevel    ─ head:Handle │ tail:Handle │ total_qty:u32 │ count:u32
Trade         ─ taker:id │ maker:id │ price(maker's) │ qty │ ts
Event (32 B)  ─ ts │ id │ price │ qty │ type{New,Cancel,Modify,Reduce} │ side │ otype │ reserved
```

## 2. Structure
```
                    ┌──────────────────────── OrderBook ────────────────────────┐
  add()/cancel() ──►│ bids_: price ↓ ─► PriceLevel ─► [o1]⇄[o2]⇄[o3]  (FIFO)    │
                    │ asks_: price ↑ ─► PriceLevel ─► [o4]⇄[o5]                 │
                    │ index_: OrderId ─► Handle   (O(1) cancel lookup)          │
                    │ pool_ : OrderPool  [ slot0 | slot1 | slot2 | ... ] ◄─────┼─ all Orders live here,
                    └────────────────────────────────────────────────────────────┘   linked by Handle
```
Orders never live in the map/hash nodes; those hold only 4-byte handles. Order payloads are
contiguous in one slab → predictable memory, no per-order `malloc`.

## 3. Event flow (replay)
```
gen_events / ITCH converter ─► events.bin ─► read_events / mmap ─► replay() ─► OrderBook
                                                                      │  ├─► Trade sink ─► stats + IStrategy::on_trade
                                                                      │  └─► IStrategy::on_event(book view)
                                                                      └─► ReplayStats {events, trades, volume, latency, checksum}
```

**Streaming replay (Phase 5b, `replay/event_stream.hpp`):** `EventLogStream` reads a log through a fixed window of records (default 2^20 = 32 MB). The header is checked against the file size at `open`, and each chunk is validated before any of its events are applied, with the same messages and absolute record indices as the other readers. `replay_stream` gives results identical to `replay()` at any window. Memory is the book plus the window, whatever the log size.

**Paced replay (Phase 5e, `replay/paced.hpp`):** event i is due at start + (ts_i − ts_0) ÷ speed. The loop sleeps for gaps over 200 µs (waking 100 µs early), then spins. Lateness (applied time − due time) is recorded against the schedule, so a backlog shows as growing lateness. Results are identical to `replay()`.

**Trading replay (Phase 5f/g, `replay_trading`, `replay/src/trading.cpp`)** is a separate loop, so `replay()`'s hot path is untouched:
```
events.bin ─► apply due gateway actions ─► apply feed event ─► deliver fills ─► ITradingStrategy::on_event(book, gateway)
                    ▲                                                │                         │
                    └──────── OrderGateway queue (arrival = send + order_latency_ns) ◄──────────┴── on_fill(fill, gateway)
```
- **Actions:** `submit`, `cancel` and `modify` are queued, never applied synchronously. An action sent at time t (the timestamp of the event or action being handled) arrives at t + latency. It is applied before the first feed event with timestamp ≥ arrival, in send order.
- **Fills:** they are delivered after the book operation that caused them finishes, so a strategy never sees a half-matched book. `Fill::remaining` is the order's exact quantity left in the book.
- **Ids:** strategy ids are ≥ 2^63 (`kStrategyIdBase`). Feed events with ids in that range are skipped and counted as rejected.
- **Metrics** (`StrategyMetrics`):
  - Counts: orders, modifies, cancels, rejects, misses, and actions still in flight at the end.
  - Quantities: fills (and how many were aggressive), filled vs offered shares, position, and cash.
  - PnL and slippage: realised PnL (average cost), marked PnL (final mid), and slippage against the mid when the order was sent (positive = cost).
- **Determinism:** same log + strategy + latency ⇒ bit-identical metrics and checksum. A strategy that sends nothing reproduces `replay()` exactly.

### Event log file format (v1 `LOBEVT01`, v2 `LOBEVT02`)
| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `LOBEVT01` or `LOBEVT02` (the last two characters are the version) |
| 8 | 8 | record count N, u64 |
| 16 | 32·N | `Event` records: `ts` u64 @0, `id` u64 @8, `price` i64 @16, `qty` u32 @24, `type` u8 @28 (0 New, 1 Cancel; v2 adds 2 Modify, 3 Reduce), `side` u8 @29 (0 Buy, 1 Sell), `otype` u8 @30 (0 Limit, 1 Market), `reserved` u8 @31 = 0 |

- All integers are little-endian. The reader `static_assert`s a little-endian host rather than swapping bytes.
- A valid file is exactly `16 + 32·N` bytes. Both readers check the count against the file size *before* allocating. They reject enum bytes out of range for the file's version (a Modify in a v1 file is invalid) and a non-zero `reserved`, with the same one-line reason.
- Cancel records leave `price`, `qty`, `side` and `otype` zero.
- Modify records (v2): `id`, the new `price` and new `qty`, and `otype` Limit. `side` is ignored, because the order keeps its own side.
- Reduce records (v2): `id`, plus `qty` = shares to remove. The other fields are zero.
- v1 and v2 share the record layout; v2 only widens the `type` range. `write_events` writes `LOBEVT01` unless a Modify or Reduce is present, so logs without them stay byte-identical to earlier versions.
- Records start at offset 16, so a page-aligned `mmap` of the file is aligned for `Event` (alignment 8).
- Any change to the record layout gets a new magic (`LOBEVT03`) rather than silently reinterpreting older files.

**Modify / Reduce semantics (Phase 5c, exchange priority rule):**
- **Modify** keeps the id and side.
  - Same price and qty ≤ current: amended in place, queue position kept (equal qty is a no-op).
  - Any other change (new price, or a higher qty): the order is removed and re-entered as a limit order at the back of its new level. It trades first if the new price crosses, and appears as the taker in those trades.
  - Unknown id: a miss. A qty of 0 is rejected and leaves the order unchanged.
- **Reduce** takes `qty` shares off a resting order and keeps its position. If `qty` ≥ what is left, the order is removed. An unknown id is a miss.
- ITCH mapping (5d, `replay/itch.hpp`):
  - The engine id is the order's original ITCH reference; the converter maps later references to it.
  - A/F become New. E/C (executions) and X (partial cancels) become Reduce. D becomes Cancel.
  - An ITCH replace U always loses priority. It becomes a Modify when the engine would also re-queue (new price or more shares), and Cancel + New with the same id otherwise.
  - Prices are ITCH units (1/10000 $) by default. `--tick 100` writes cents: any price that isn't a whole cent fails the conversion, unless `--drop-off-grid` drops those orders (counted). Real sub-dollar orders can be sub-penny. Timestamps are ns since midnight.
  - Consequence for strategies: real aggressors never reach the engine, so a strategy's resting order is filled only when a displayed Add crosses it. There is no queue-position model yet (see the Phase 5(d) note).

**Golden log:** `tests/data/golden_2000_seed7.bin` (`gen_events … 2000 7`) is committed together with its expected results: checksum `c46a281df7e17679`, 576 trades, volume 14,366, 109 cancel misses, 687 resting orders. Every storage variant must reproduce them. A separate test checks that the generator still produces the file byte-for-byte, so a generator change and a matching-behaviour change fail different tests.

**Golden v2 log:** `tests/data/golden_v2_2000_seed7.bin` (`gen_events … 2000 7 15`, so 15% Modify/Reduce: 1343 New, 339 Cancel, 231 Modify, 87 Reduce). Expected: checksum `91319d83ac94152c`, 489 trades, volume 12,124, 98 cancel / 68 modify / 21 reduce misses, 593 resting orders. These are checked on all 8 variants and both readers, with the same generator-reproduction test.

## 4. Invariants (assert these in `validate()` — Phase 7)
1. Every resting order is in exactly one level's list and exactly once in `index_`.
2. Book is never crossed after an operation: `best_bid < best_ask` (when both exist).
3. `level.total_qty == Σ order.qty` and `level.count == list length`; empty levels are erased.
4. Within a level, list order == arrival order (time priority).
5. Fills execute at the maker's price; taker never trades through a better price.
6. `pool.live() == index_.size()`; free list + live == capacity.
7. Market orders never rest; qty==0 and duplicate ids are rejected.
8. Determinism: identical event sequence ⇒ identical trades and identical `checksum()`.
9. Amendments: a Reduce, or a Modify that keeps the price without raising the qty, never changes an order's queue position. Any other Modify moves it behind every order already at its new level.

## 5. Design decisions (and what you'd change at scale)
| Decision | Why | Revisit when |
|---|---|---|
| Integer ticks | exact, comparable, no FP error | never |
| 32-bit handles, not pointers | half the link size, slab locality | pool > 4B orders |
| Intrusive doubly-linked FIFO | O(1) cancel & pop-front, no alloc | queue positions need random access |
| `std::map` levels (baseline) | simple & correct first | Phase 3 → ladder + bitmap. Dense (band anchored on the first price) now wins by 14–22% on real ITCH flow in cents. Decision (2026-09-28): the tree stays default until a re-centring band has been measured; then dense becomes default |
| `unordered_map` index (baseline) | simple & correct first | Phase 3 → flat table. Since 2026-09-28 its home slot keeps blocks of 64 sequential ids together and Fibonacci-scatters the blocks. The identity hash collapsed after about 10M events (linear-probing clusters) |
| Fixed-capacity pool | no allocation/latency spikes | grow-by-chunk if needed |
| Single-threaded engine | no locks; determinism | scale by symbol sharding, not by threads per book |
| Sink template param | zero-cost callback (inlined) | virtual sink only in cold paths |
| Conservative reject when pool full | keeps `add` all-or-nothing | reserve slots differently |
