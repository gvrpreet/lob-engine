# Architecture & schema

## 1. Data model
```
OrderId  u64        Price  i64 (ticks)     Qty  u32      Handle u32 (pool index)

Order (32 B)  ─ id:u64 │ price:i64 │ qty:u32 │ prev:u32 │ next:u32 │ side:u8 │ pad
PriceLevel    ─ head:Handle │ tail:Handle │ total_qty:u32 │ count:u32
Trade         ─ taker:id │ maker:id │ price(maker's) │ qty │ ts
Event (32 B)  ─ ts │ id │ price │ qty │ type{New,Cancel} │ side │ otype │ reserved
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

### Event log file format (v1, `LOBEVT01`)
| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `LOBEVT01` (the last two characters are the version) |
| 8 | 8 | record count N, u64 |
| 16 | 32·N | `Event` records: `ts` u64 @0, `id` u64 @8, `price` i64 @16, `qty` u32 @24, `type` u8 @28 (0 New, 1 Cancel), `side` u8 @29 (0 Buy, 1 Sell), `otype` u8 @30 (0 Limit, 1 Market), `reserved` u8 @31 = 0 |

- All integers are little-endian. The reader `static_assert`s a little-endian host rather than swapping bytes.
- A valid file is exactly `16 + 32·N` bytes. `read_events` checks the count against the file size *before* allocating, and rejects out-of-range enum bytes or a non-zero `reserved`, with a one-line reason.
- Cancel records leave `price`, `qty`, `side` and `otype` zero.
- Records start at offset 16, so a page-aligned `mmap` of the file is aligned for `Event` (alignment 8).
- Any layout change (e.g. a Replace event that needs a second id) gets a new magic, `LOBEVT02`, rather than silently reinterpreting v1 files.

**Golden log:** `tests/data/golden_2000_seed7.bin` (`gen_events … 2000 7`) is committed together with its expected results: checksum `c46a281df7e17679`, 576 trades, volume 14,366, 109 cancel misses, 687 resting orders. Every storage variant must reproduce them. A separate test checks that the generator still produces the file byte-for-byte, so a generator change and a matching-behaviour change fail different tests.

## 4. Invariants (assert these in `validate()` — Phase 7)
1. Every resting order is in exactly one level's list and exactly once in `index_`.
2. Book is never crossed after an operation: `best_bid < best_ask` (when both exist).
3. `level.total_qty == Σ order.qty` and `level.count == list length`; empty levels are erased.
4. Within a level, list order == arrival order (time priority).
5. Fills execute at the maker's price; taker never trades through a better price.
6. `pool.live() == index_.size()`; free list + live == capacity.
7. Market orders never rest; qty==0 and duplicate ids are rejected.
8. Determinism: identical event sequence ⇒ identical trades and identical `checksum()`.

## 5. Design decisions (and what you'd change at scale)
| Decision | Why | Revisit when |
|---|---|---|
| Integer ticks | exact, comparable, no FP error | never |
| 32-bit handles, not pointers | half the link size, slab locality | pool > 4B orders |
| Intrusive doubly-linked FIFO | O(1) cancel & pop-front, no alloc | queue positions need random access |
| `std::map` levels (baseline) | simple & correct first | Phase 3 → ladder + bitmap |
| `unordered_map` index (baseline) | simple & correct first | Phase 3 → flat table |
| Fixed-capacity pool | no allocation/latency spikes | grow-by-chunk if needed |
| Single-threaded engine | no locks; determinism | scale by symbol sharding, not by threads per book |
| Sink template param | zero-cost callback (inlined) | virtual sink only in cold paths |
| Conservative reject when pool full | keeps `add` all-or-nothing | reserve slots differently |
