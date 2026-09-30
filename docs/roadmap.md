# Roadmap: learning systems by building an order book

Each phase = **Learn → Build → Verify → Exit criteria**. Do not start a phase until the previous
one's exit criteria are met. Phase 1 code already exists; read it, break it, and re-derive it
before optimising. Suggested pace: ~1 phase/week (part-time), 8-10 weeks.

**Golden rule:** no optimisation lands without (1) differential tests green, (2) checksum unchanged
on the replay, (3) a benchmark-log entry showing the delta, (4) a perf counter that *explains* it.

---
## Phase 0 — Toolchain & habits  (2–3 days)
**Learn:** CMake targets/presets, compile flags (`-O2/-O3`, `-g`, `-fno-omit-frame-pointer`), how ASan/UBSan/TSan work, reading compiler warnings.
**Build:** get `scripts/check.sh` green on your machine; open the project in clangd (uses `compile_commands.json`).
**Verify:** deliberately introduce a use-after-free in `OrderPool` and confirm ASan catches it. Break FIFO order and confirm a test fails.
**Exit:** you can build under 4 configs and explain what each catches.

## Phase 1 — Correct baseline  (≈1 week)  ✅ scaffolded
**Learn:** what an order book is (bids/asks, spread, depth); price-time priority; market vs limit; trade at maker's price; integer ticks vs floats; C++20 features used (`contains`, defaulted `operator==`, `std::span`, concepts-free templates).
**Build (re-implement from a blank file, then diff against `engine/include/lob/`):**
`Order`, `PriceLevel` (intrusive list), `OrderBook::{add,cancel,match}`.
**Verify:** `tests/unit/test_order_book.cpp`; write 5 more edge cases (self-cross, market on one-sided book, cancel during partial fill, huge qty, id reuse after fill).
**Exit:** all tests pass; you can whiteboard the match loop and prove each invariant in `architecture.md`.
**Status (2026-09-26):** Unit, differential, and replay checks pass; added crossing, one-sided market, cancel-after-partial-fill, maximum-quantity, and filled-id-reuse tests. The requested participant self-cross case cannot currently be represented because orders have no trader identity. The invariant walkthrough is not established by build checks alone, so Phase 1 exit evidence remains partial.
**Read:** WK Selph, *How to Build a Fast Limit Order Book*; Nasdaq TotalView-ITCH 5.0 spec (skim message types).

## Phase 2 — Memory & cache-aware layout  (≈1 week)  ✅ partially scaffolded
**Learn:** cache lines (64B), L1/L2/L3 latency, TLB & huge pages, AoS vs SoA, false sharing, why pointers chase = cache misses, `alignas`, `static_assert(sizeof)`, allocation cost of `new`/`malloc`.
**Build:** (a) measure baseline with `std::allocator` per order (write `NaiveOrderBook` variant with `new Order`) vs slab `OrderPool`; (b) experiment: 32B vs 64B vs 48B `Order`; (c) hot/cold split (move rarely-used fields out); (d) pre-fault pool memory (`madvise`/touch pages), try `MAP_HUGETLB`/THP.
**Verify:** cachegrind/`perf stat` L1-dcache-load-misses per event, before vs after.
**Exit:** you can explain in numbers *why* handles beat pointers and slab beats malloc here.
**Status (2026-09-26):** (a) allocator baseline, (b) 32B/48B/64B size comparison, and (c) hot/cold split implemented and measured. For (d), the basic pool prefault/touch path is implemented; huge-page behavior has not been experimentally evaluated. The hot/cold experiment did not show a reliable timing or cache-miss improvement over the 32B AoS default. Native timing spread is wide, and this WSL2 kernel does not provide `perf` counters. Phase 2 exit evidence remains partial; see the Phase 2 experiment records.
**Deferred checkpoint for (d):** Revisit THP (`MADV_HUGEPAGE`) or explicit `MAP_HUGETLB` only on a representative Linux host with usable performance counters and a workload large enough to create meaningful TLB pressure. Compare prefault-only against huge-page requests, record latency/throughput and TLB-related counters, and keep huge pages optional unless results show a repeatable benefit. This investigation is not a prerequisite for continuing to Phase 3.
**Status (2026-09-27, native):** System-wide THP `always` vs `never` on the 5M log gave dTLB misses 1.5M → ~10k and +5–6% timed throughput, reproduced in two runs. `perf stat` also explains (a)–(c): slab via fewer instructions and LLC misses; record size via dTLB misses; hot/cold slightly worse (more LLC misses). Still open: prefault-only vs pool-level `MADV_HUGEPAGE`, which needs a CLI path to `prefault_pool()`.
**Update (2026-09-27):** Built `HugePageOrderBook` (`replay_cli --huge-pages`): pool + id index on 2 MiB `MADV_HUGEPAGE` storage, default path unchanged. Prefault-only needs no flag, because the pool constructor already faults every page. Waiting on native counters (collected by every `native_run.sh` mode).
**Read:** Drepper, *What Every Programmer Should Know About Memory* (§3, §6); Agner Fog, *Optimizing software in C++* (memory chapters).

## Phase 3 — Faster data structures  (1–2 weeks)  ← the main "cache-aware" payoff
**Learn:** open addressing vs chaining, Robin Hood hashing, tick-indexed ladders, bitset + `std::countr_zero` for "next non-empty level", branch prediction.
**Build (one at a time, benchmark each):**
1. Replace `std::unordered_map<OrderId,Handle>` with a flat open-addressing table (power-of-two, linear probing, tombstone-free deletion via backward shift).
2. Replace `std::map` price levels with a dense array indexed by `(price - base)` + occupancy bitmap (+ 2-level bitmap for wide ranges). Cache `best_bid/best_ask`.
3. Optional: sorted `std::vector` of levels (best at the back) — compare to the ladder for realistic depth (~10–50 active levels).
**Verify:** differential test + replay checksum identical after every swap.
**Exit:** ≥ measurable throughput gain per swap, each explained by a counter (fewer LLC misses, fewer branch-misses, fewer instructions).
**Status (2026-09-26):** Phase 3(a-c) experiments are implemented and behaviorally verified. The flat index and dense levels show promising medians, while sorted levels did not improve the synthetic run; ranges overlap and WSL2 hardware counters are unavailable. Keep `std::map` as default. Phase 3 exit evidence remains partial; next focus is Phase 4 measurement quality/native profiling.
**Status (2026-09-28, real data):**
- On Nasdaq ITCH order flow (AAPL, AMD and TQQQ, 2020-01-30, prices in cents, dense band anchored on the instrument), dense levels beat the tree by 14–22%, and every dense run beats every tree run. Sorted ties or loses.
- Cachegrind explains it: −16% instructions and −41% branch mispredictions per event, with LLC misses unchanged.
- The earlier fixed 9,000–11,000 band only fitted the synthetic generator.
- **Decision (user, 2026-09-28):** keep the tree (`std::map`) as the default. Make dense the default once a band that re-centres itself has been measured on the same ITCH logs. See `experiments/2026-09-28-dense-levels-itch.md`.

**Status (2026-09-28, dense is the default):** the re-centring band was measured.
- It matches the anchored band where that fits (AAPL/AMD/TQQQ) and wins +22% on TSLA, where the price left the anchored band (−51% mispredicts).
- Per the decision, dense levels are now the default (`8ac27d2`): +14–27% over tree on all four real symbols with no overlap; synthetic 1M/5M within noise (+1% / −6%).
- The tree remains available as `TreeOrderBook` / `--tree`.
- The hot-loop baseline is now 361.5 I/event (v1 1M). See `experiments/2026-09-28-dense-recentre-default.md`.

**Status (2026-09-27, native):** Counters now explain (b) and (c). Dense levels halve branch misses (IPC 1.33 vs 0.96) despite more instructions, and win 14–23% where depth is bounded; sorted levels lose on instruction count. (a) still lacks a counter comparison, because the `unordered_map` variant was removed. `std::map` stays default.

## Phase 4 — Measurement discipline  (parallel to phases 2–3, formalise here)
**Learn:** microbenchmark pitfalls (dead-code elimination, warm-up, frequency scaling, noisy neighbours), percentiles vs means, coordinated omission, `rdtsc`/`chrono` overhead, `perf stat/record`, flame graphs, `taskset`, isolcpus.
**Build:** follow the protocol in `workflow.md`; add an `rdtscp`-based timer; build a latency histogram (HDR-style log buckets) replacing the raw vector.
**Status (2026-09-26):** Opt-in final/peak level telemetry and deterministic 10/25/50-level streams are implemented with and without market trades. At 25% configured trade rate, exact depths held with about 200k trades per 1M events. Sorted medians trailed tree at all depths; dense medians were higher at 25/50 but ranges overlapped. Keep `std::map` default; repeat on native Linux if counters are available.
**Status (2026-09-27):** Both Build items are done. An HDR-style log-bucket histogram replaced the raw latency vector. It is exact below 64 ns and within 1/32 above, and its percentiles match the old exact values. An opt-in calibrated `rdtscp` timer was added (`ReplayConfig::latency_timer`, `replay_cli --tsc`), and `bench_match` reports each timer's overhead. On WSL2 (clocksource `tsc`) both timers cost ~20 ns, and rdtscp's `lfence` lowers throughput, so `steady_clock` stays the default. The histogram removed the measurement's simulated cache misses, but wall-clock differences overlap. Still open: the Verify step (<5% variance across days on an idle machine) and the Exit criterion. Both need a native host. The native-host plan and prerequisites are in `guides/native-linux-checklist.md`.
**Verify:** run the same benchmark 3 days apart on an idle machine; variance < ~5%.
**Exit:** every number in your benchmark log has hardware, compiler, flags, commit, and run count.
**Read/watch:** Chandler Carruth, *Tuning C++: Benchmarks, and CPUs, and Compilers!* (CppCon 2015); Brendan Gregg, perf examples & flame graphs; Google Benchmark user guide.

## Phase 5 — Replay framework  (≈1 week)  ✅ baseline scaffolded
**Learn:** deterministic simulation, event sourcing, binary formats, `mmap`, endianness/padding, market data (ITCH/PITCH: add/execute/cancel/replace).
**Build:** (a) `mmap`-based reader (zero-copy `span<const Event>`); (b) streaming replay for logs bigger than RAM; (c) add **Replace/Modify** event; (d) ITCH-subset parser → `Event` converter (use a public sample file); (e) speed control (as-fast-as-possible vs timestamp-paced); (f) richer `IStrategy` API (submit orders back into the book with simulated latency); (g) PnL/fill-rate/slippage metrics.
**Verify:** same log ⇒ same checksum, always. Golden-file test: commit a small log + expected checksum.
**Exit:** replay 5M+ events end-to-end and report throughput, trades, and strategy metrics; results reproducible bit-for-bit.
**Status (2026-09-27):** Verify's golden-file test is done: a committed 2,000-event log with pinned checksum, trades, volume, cancel misses and resting orders, checked on all 8 storage variants, plus a separate check that the generator still produces it. `read_events` now validates the format (size vs header count before allocating, enum/reserved bytes) and reports why a file is rejected. The v1 format is specified in `architecture.md`.
**Status (2026-09-27, 5a):** `MappedEventLog` maps a log read-only (`MAP_POPULATE`), runs the same validation, and hands replay a zero-copy view of the records. It is `replay_cli`'s default (`--read` keeps the copying reader) and prints the load time. Both readers give identical golden checksums and identical rejection reasons. WSL2 load of the 5M log, one informal run: mmap 53 ms vs read 95 ms. Native numbers come from the `mmap` perf variant. Next: (c) Replace/Modify, which needs a semantics decision and format v2.
**Decision (2026-09-28, 5c):**
- Same-id Modify: the 32 B record is unchanged, with new types `Modify`/`Reduce` under magic `LOBEVT02`.
- Exchange priority rule: same price with lower qty keeps queue position; a price change or qty increase re-queues the order (it may trade). Reduce = partial cancel that keeps position (ITCH 'X').
- The ITCH converter (d) maps ITCH's replace (old ref → new ref) onto the same engine id.
Order after (c): (f)+(g), (d), (b), (e).
**Status (2026-09-28, 5c):** Done.
- Engine: `modify`/`reduce` are in the engine and in the oracle, and the differential test (3 seeds, all 8 variants) includes them.
- Format and golden files: v2 logs (`LOBEVT02`), the replay loop, and an opt-in generator (`gen_events … <modify%>`). Golden v2 `91319d83ac94152c` is pinned on all variants and both readers. v1 files and checksums are byte-identical.
- Cost: the first version cost +19% instructions/event on v1 logs by breaking inlining. After the fix (dispatch in its own TU), v1 is −0.5% vs base (Cachegrind).
- Next: (f)+(g), strategy order entry and metrics.
**Status (2026-09-28, 5f+g):** Done.
- `replay_trading` (its own loop and TU) with a latency-delayed `OrderGateway` (submit/cancel/modify), fills delivered after each book operation, and `StrategyMetrics`: fills, fill rate, position, cash, realised/marked PnL, and slippage vs mid at send.
- `MarketMaker` (joins the touch, position limit) is available as `replay_cli --strategy mm [--order-latency ns]`. Its golden result is pinned, and a silent strategy reproduces `replay()` on both golden logs.
- Plain replay I refs are unchanged. The market maker's PnL on the synthetic feed is an artefact (no adverse selection); ITCH data (d) is the meaningful workload.
- Next: (d).
**Status (2026-09-28, 5d):** Done. `itch_convert` turns one symbol of a Nasdaq ITCH 5.0 day (2020-01-30, 12.95 GB) into a v2 log in about 4.3 s.
- AAPL, AMD and TQQQ rebuild exactly: 0 unknown refs, 0 misses, 0 engine trades. Mid-session checksums agree on all variants.
- Two follow-ups before real-data results mean anything:
  1. **Dense levels:** the band is hard-coded to the synthetic range, so every ITCH level overflows. Anchor the band to the instrument and use real ticks, then redo tree vs dense vs sorted.
  2. **Strategy fills:** they need a queue-position model. The market maker currently loses about $3.9k on AAPL through one-sided, adverse-only fills.
- Next: (b).
**Status (2026-09-28, 5b):** Built.
- `EventLogStream` reads through a fixed window and validates each chunk before use; `replay_stream` runs its own loop; `replay_cli --stream [--window N]`.
- Results are identical to full-load replay at any window, including 1.
- Memory on the 5M log: 69–102 MB vs 225 MB for mmap/read.
- The 50M attempt exposed a default-book bug. The flat order index's identity hash collapsed replay after about 10M events (0.41M ev/s at 15M). It is fixed with 64-id blocks, Fibonacci-scattered (`b47f506`): no cliff up to 50M, and a 12–17% cost on 1M–5M logs from hash arithmetic. See `experiments/2026-09-28-index-hash-clustering.md`.
- 50M per-reader run (user, `9b37878`): streaming holds 67–99 MB at both 5M and 50M, vs 1,593 MB for mmap/read at 50M, for 11–15% lower throughput. Checksums are identical in every mode.
**Status (2026-09-28, 5e):** Done.
- `replay_paced` / `replay_cli --paced <speed>` applies events on a wall-clock schedule from their timestamps, with results identical to `replay()`.
- It reports lateness against the schedule: the backlog shows when the demanded rate exceeds about 16M ev/s.
- On AAPL it exposes ITCH microbursts (1.8% of events more than 1 ms late at 10,000× speed).
- Next: the Phase 5 exit review.

**Phase 5 exit: met (2026-09-28, commit `b7971e7`, `scripts/phase5_exit.sh`).**
- Five logs of 2M–50M events, each replayed end-to-end through mmap, read and stream (with the VWAP strategy) and through the trading replay with the market maker at 1 µs order latency: the synthetic 5M and 50M, and ITCH AAPL, AMD and TQQQ in cents.
- Both runs matched bit-for-bit on all 20 outputs (timing lines excluded), and the three readers agree on every log.
- 50M synthetic: 29,298,045 trades at 13.2M ev/s, checksum `e45d178766abc830`, VWAP 10,109.923.
- Market maker on 50M: 144,156 fills, fill rate 0.9987, marked PnL 508,148 ticks×shares, slippage −1.07 ticks/share.
- On ITCH, the market maker's PnL is negative because of one-sided fills (see 5d), so it is reported as a metric, not as a strategy result.
- The golden files (v1, v2 and the market maker) pin the same behaviour in `check.sh`.

**Follow-up (2026-09-28): queue-position fills.**
- Strategy orders on ITCH data are now filled by the feed's own executions whenever price-time priority puts them ahead of the executed order: the new Execute event (format v3), on by default in the trading replay.
- Fills become two-sided (1.5–2× more on AAPL, AMD and TSLA). The market maker still loses because it treats its own order as the touch (follow-up 3).
- See `experiments/2026-09-28-queue-position-fills.md`.

**Follow-up (2026-09-28): the market maker quotes at the others' touch.** It skips a level where only its own order rests.
- Losses fall 4–60×: the removed fills were the stale, picked-off ones. The residual is noise around slightly negative, as expected from a signal-free touch-joiner.
- The strategy and simulator now support quoting a result with its caveats. See `experiments/2026-09-28-market-maker-others-touch.md`.

## Phase 6 — Concurrency (optional stretch)  (1–2 weeks)
**Learn:** memory model, `std::atomic` orderings, cache-coherence traffic, SPSC ring buffers, false sharing, why matching engines are single-threaded (sequencer pattern), LMAX Disruptor idea.
**Build:** SPSC queue (reader thread → engine thread), padding with `alignas(64)`, cached head/tail indices.
**Verify:** TSan clean; throughput vs a mutex queue; same checksum as single-threaded replay.
**Read:** Anthony Williams, *C++ Concurrency in Action*; Carl Cook, *When a Microsecond Is an Eternity* (CppCon 2017); David Gross, *Trading at Light Speed* (Meeting C++ 2022).

## Phase 7 — Hardening & polish  (≈1 week)
**Build:** libFuzzer/AFL harness over the op stream (crash + oracle compare); invariant checker (`OrderBook::validate()` walking lists/levels/index) called in debug builds; `clang-tidy` in CI; property test "sum of maker fills == sum of taker fills"; add order types (IOC/FOK/post-only) as extension.
**Exit:** 10M-op fuzz run clean under ASan; README has architecture diagram + benchmark table + reproducible commands.
**Status (2026-09-28): exit met.**
- `OrderBook::validate()` implements invariants 1, 2, 3 and 6, plus link, order and level checks. It is called from the differential tests and after every fuzz operation.
- A libFuzzer harness runs the dense and tree books against the naive oracle: 21.2M operations clean under ASan/UBSan.
- clang-tidy (warnings as errors) runs in CI, together with a 60 s fuzz job. It found two unchecked `rewind` calls, now fixed.
- The README has an architecture diagram, a results table and tested commands.
- Mutation checks: `validate()` and the fuzzer each caught a deliberately injected bug.
- Maker/taker fill consistency is covered by the field-by-field oracle comparison.
- Open extensions: IOC/FOK/post-only order types, and `validate()` inside replay loops in debug builds. See `experiments/2026-09-28-phase7-hardening.md`.

---
## Resume evidence map (fill from `benchmark-log.md`, never from guesses)
| Claim | Evidence to produce | Evidence so far (2026-09-28; details in `benchmark-log.md`) |
|---|---|---|
| price-time priority matching (market/limit/cancel) | unit tests + differential test vs oracle | 91 tests. Differential vs the naive oracle on 3 seeds and all 8 storage variants, including Modify/Reduce with the exchange priority rule. A mutation test is caught. |
| cache-aware order storage | `static_assert(sizeof(Order)==32)`, slab pool, misses/event before→after (Phase 2–3) | 32 B `Order` asserted. Native `perf`: slab vs `new` 1.77–1.86 vs 2.90–3.05 cache-misses/event. Dense levels on real ITCH flow: +14–22% vs tree, −41% mispredicts (Cachegrind). |
| low-latency matching | p50/p99/p99.9 ns from pinned benchmark, with hardware + flags | Native (Ryzen 7 7840HS, live USB): 1M p50/p99/p99.9 = 40/151/263 ns (20 ns timer overhead included). Cross-day variance check still open (days 2–3). |
| replay framework, millions of events | `replay_cli` on ≥ 5M-event log: events/s, deterministic checksum | 50M events at 13.2M ev/s, bit-for-bit across runs and readers. Streaming holds 67–99 MB vs 1.6 GB. Real Nasdaq ITCH days rebuilt exactly (0 misses, 0 crossed). |
| strategy evaluation | ≥ 1 strategy (e.g. VWAP/market-maker) with PnL/fill metrics in Phase 5(g) | Market maker via a latency-delayed gateway: fills, fill rate, position, realised/marked PnL, slippage; pinned golden result. Caveat: the ITCH fill model is one-sided (no queue-position model yet). |
