# Development Journey and Decision Log

Updated: 2026-09-27

## Why This Exists

This document keeps the reasoning trail of the order-book project in one place. The roadmap says what to build; the benchmark log records measurements; experiment notes capture individual trials. This journal connects them: what the idea was, what became difficult or unclear, why a decision was made, what was postponed, and what should be considered next.

Keep observed results separate from hypotheses. A benchmark result is evidence for its workload and environment, not a universal claim. When a phase advances, add the problems and decisions that actually occurred rather than filling gaps with hindsight.

## Current Position

Phase 3(a-c) structural experiments are implemented and behaviorally verified. Phase 4 has controlled-depth workloads (with and without trades), a log-bucket latency histogram, and an opt-in `rdtscp` timer. Native Linux day 1 (2026-09-27, live USB) supplied hardware counters for most structural swaps, a system-wide THP comparison, and a passing TSan run. Two more days are needed for the cross-day variance check. Tree remains default; the pool-level huge-page trial for Phase 2(d) still needs a CLI path.

## Phase 0: Toolchain and Habits

**Original idea:** Establish a repeatable C++ build, test, sanitizer, and benchmark workflow before optimizing the engine.

**What happened:** The repository was scaffolded with CMake presets, a lightweight test harness, debug/release/sanitizer builds, and scripts for checks and benchmarks. This established a local baseline and made subsequent changes testable.

**Problems and confusion:** Sanitizer availability does not guarantee that every sanitizer runtime works in the target environment. The WSL2 setup later proved unable to run the ThreadSanitizer runtime reliably, and it does not expose usable hardware `perf` counters.

**Decision and reason:** Keep build and correctness checks reproducible, but label unsupported runtime or hardware measurements instead of treating missing evidence as a pass or a performance result.

**Left to revisit:** Recheck TSan and hardware-counter availability on a native Linux host. Record exact host/compiler/flags for every future measurement.

**Update (2026-09-27):** On a native live-USB host, `perf` counters work. TSan first aborted with "unexpected memory mapping". This was not a code problem: kernel 7.0's 32-bit mmap ASLR entropy is beyond what GCC 13's TSan runtime supports. Running the tests under `setarch -R` fixed it, and the suite passed.

**Ideas to carry forward:** Keep correctness and measurement scripts routine so optimization work cannot silently skip them.

## Phase 1: Correct Baseline

**Original idea:** Build a simple price-time-priority limit order book first, then understand its invariants before changing data structures.

**What happened:** The baseline uses price levels ordered by `std::map`, an `OrderId` to pool-handle index, and slab-backed order storage. Unit, differential, and replay tests were added, including crossing, one-sided market orders, partial-fill cancellation, large quantities, and filled-ID reuse.

**Problems and confusion:** A requested self-cross case cannot be represented without participant/trader identity on orders. A green test suite does not by itself prove that every invariant is understood; the architecture walkthrough remains incomplete evidence. Early test assumptions also needed checking against the actual resting state rather than a guessed order count.

**Decision and reason:** Do not invent participant semantics just to force a self-cross test. Keep the missing identity as an explicit model limitation. Use differential tests and checksums for behavior, while treating the invariant walkthrough as a separate learning task.

**Left to try later:** Add trader identity only if a later market model or risk feature needs self-trade prevention. Complete and review the invariant walkthrough; add a `validate()` helper if it can check book/index/pool consistency without distorting production behavior.

**Ideas to carry forward:** Preserve deterministic replay and a simple reference implementation as the oracle for future structural swaps.

## Phase 2: Memory and Cache-Aware Layout

**Original idea:** Measure per-order allocation against a slab, compare record sizes, try hot/cold separation, and investigate prefaulting/huge pages. Keep the default layout only if measurements justify it.

**What happened:**

- **2(a), allocator versus slab:** Both a per-order allocator baseline and the slab pool were measured. A corrected matched-work profile showed the slab faster in the tested synthetic workload, though the absolute timings varied considerably. The corrected experiment record supersedes the earlier unmatched profile.
- **2(b), record size:** 32B, 48B, and 64B order records were compared with equivalent replay checksums. Timing ranges overlapped; no alternate size earned a default change.
- **2(c), hot/cold fields:** The split was functionally equivalent, but timing and simulated cache-miss results did not show a reliable improvement over 32B AoS.
- **2(d), warm pages:** A pool prefault/touch path and Linux `MADV_WILLNEED` request were added as an opt-in warm-page experiment. It is functionally safe and does not alter book state.

**Problems and confusion:** Native timing spread was wide. Cachegrind numbers are simulated, not hardware counters. WSL2 did not provide usable `perf` counters, limiting explanations about actual cache/TLB behavior. A working prefault path is not evidence that huge pages help.

**Decision and reason:** Retain the 32B AoS slab as the default because no layout variant showed a repeatable enough advantage. Treat prefaulting as an optional experiment, not an always-on optimization.

**Left to try later:** Evaluate THP (`MADV_HUGEPAGE`) or explicit `MAP_HUGETLB` only on a representative Linux host with TLB-related counters and a workload large enough to create TLB pressure. Compare against prefault-only; keep huge pages optional unless the benefit is repeatable. See the checkpoint in `roadmap.md`.

**2(d) revisited (2026-09-27):** Native system-wide THP showed the TLB matters (dTLB misses 150× lower, +5–6%). A book-only huge-page variant (`HugePageOrderBook`, `--huge-pages`) now separates the book's share from the event buffer's. Two things came out of building it:
- The existing `prefault()` was a no-op for the slab, because the constructor's free-list loop already touches every page.
- A first draft with a runtime flag in a stateful allocator added ~1.7 instructions per event to the *default* book. Making it a separate type kept the default code identical. This is the same lesson as the latency histogram: changing the default types changes their code generation.

Native counters pending; see `2026-09-27-phase2d-huge-pages.md`.

**Ideas to carry forward:** Require identical checksums for every layout; report min/median/max and environment; do not choose a default based on a small median difference inside noisy ranges.

## Phase 3: Faster Data Structures

**Original idea:** Reduce pointer chasing and per-operation work by replacing the order-ID map, then price-level map, one structure at a time. Keep each swap behaviorally equivalent and benchmark it independently.

### Phase 3(a): Flat Order-ID Index

**Idea:** Replace `std::unordered_map<OrderId, Handle>` with a power-of-two open-addressing table, linear probing, and tombstone-free deletion through backward shifting.

**Problems and confusion:** Deletion is the risky part: clearing an entry naively can break lookup for entries later in a collision cluster, particularly when the cluster wraps across the end of the table. The table capacity also must be sized so the maximum number of resting orders cannot fill every slot.

**Decision and reason:** Use a fixed-capacity table larger than the pool's maximum live-order count, and backward-shift deletion. Add a regression that creates a wrapped collision cluster, deletes from its middle, then verifies later entries remain findable and the freed space can be reused.

**Evidence:** Differential tests and replay checksums pass unchanged (`ba7911b3a1dd1d8a`). Three seven-run benchmark invocations showed higher synthetic throughput than the prior map implementation. Cachegrind showed fewer instruction and data references but slightly more simulated D1 and last-level data misses. Since hardware counters are unavailable and timing spread remains material, record the throughput as promising rather than conclusive; do not claim a cache-miss reduction. Details are in `2026-09-26-phase3a-flat-id-index.md` and `benchmark-log.md`.

**Left to try later:** Recheck on native Linux with `perf` branch/cache counters. Exercise different ID distributions and larger live-order counts; watch memory overhead from reserving a flat table above pool capacity.

### Phase 3(b): Dense Price-Level Experiment

**Starting idea:** Compare the current `std::map<Price, PriceLevel>` with a dense price-indexed array and occupancy bitmap, potentially with a second-level bitmap for wide ranges. Cache best bid/ask only if it simplifies or speeds the hot path without creating fragile duplicated state.

**Questions to answer before/during implementation:** What price range is realistic and bounded? How should sparse or out-of-range prices be represented? What workload depth is representative? Does bitmap scanning beat the map for the measured level counts? What invariants keep cached best prices synchronized after add, cancel, and full fill?

**Decision rule:** Make this a separate swap from the ID index. Preserve replay equivalence and differential tests. Do not choose the dense ladder unless realistic workloads show a repeatable, explainable gain. Record rejected alternatives and boundary assumptions here after testing.

**What happened:** Added an opt-in dense band for ticks 9,000-11,000 with an ordered overflow map so signed 64-bit prices outside the experiment range keep working. Matching, cancellation, best-price queries, and checksums share a level-store interface; the tree store remains the default.

**Problems and confusion:** A fixed dense range alone would reject valid prices or allocate impractically large spans. The overflow path preserves semantics, but introduces a second structure whose best level must be merged correctly with bitmap levels. Best-price lookup scans occupancy words; a summary bitmap may be worth testing.

**Evidence and decision:** Differential, replay-equivalence, boundary, and extreme-price tests pass with identical checksum `ba7911b3a1dd1d8a`. Three seven-run synthetic samples had higher dense medians (32.15-34.56M vs 29.70-30.91M events/s), but min/max ranges overlap and hardware counters are unavailable. Keep dense levels experimental; do not change the default.

**Left to try later:** Repeat with native Linux branch/cache counters, broader price distributions, deeper books, and explicit overflow-rate reporting. See the Phase 3(b) experiment note.
### Phase 3(c): Sorted-Vector Trial (Evaluated)

**Initial idea:** Compare a sorted vector with the best price at the back against tree and dense level stores for shallow books (about 10-50 active levels).

**What happened:** Added a separate sorted-vector book/replay/benchmark backend. Lookup uses binary search; insertion and erase shift entries. Best-at-back ordering is comparator-aware for both bids and asks.

**Problems and confusion:** The original benchmark did not report active-level count, so the sorted-vector result could not confirm the shallow-book premise. Phase 4 added opt-in shape metrics; the standard 1M stream reached 135 peak levels. Controlled shallow-book workloads are still needed. Vector insertion/erase may outweigh locality gains.

**Evidence and decision:** Direct ordering/cancel/fill tests, a 100k-operation differential run, and replay equivalence pass with checksum `ba7911b3a1dd1d8a`. Three seven-run samples gave sorted medians of 21.63-21.83M events/s versus tree 22.62-23.06M and dense 23.43-24.31M. Ranges overlap; sorted did not win this workload. Keep all non-tree stores experimental.

**Left to try later:** Controlled 10/25/50-level runs are now complete, but their generator uses only adds and cancels. The sorted backend had lower medians than tree at all depths, with overlapping ranges. Add controlled matching activity before concluding how the structures compare under fills.
## Phase 4: Measurement Discipline

**Planned idea:** Improve benchmark trustworthiness with a more disciplined protocol, better latency histograms, and potentially an `rdtscp` timer for cases where `steady_clock` overhead dominates.

**Journey so far:** Pinned multi-run benchmarks record ranges, but WSL2 governor visibility and hardware counters are limited. Replay optionally reports depth from a separate untimed pass. The standard 1M stream peaks at 135 levels. Controlled add/cancel and 25%-configured market-trade workloads now preserve exact 10/25/50 levels; the latter produces about 200k trades per 1M events.

**Controlled-depth results:** In add/cancel-only runs, sorted medians trailed tree at all depths; dense medians were higher at 25/50 but all ranges overlapped. With 25%-configured trade flow, sorted again trailed tree; dense medians were higher at 25/50 and ranges still overlapped. Checksums and exact depths held. WSL2 has no counters to explain the deltas; see the two Phase 4 experiment notes.
**Problems/decisions/deferred work:** This phase is not complete. Do not claim sub-100ns latency from a timer with comparable overhead. Keep event generation outside the timed region, preserve checksums, and record machine/compiler/flags/run count. Re-run important results on an idle native host and across separate days before calling them stable.

### Phase 4: Timer and Latency Histogram (2026-09-27)

**Idea:** The roadmap's remaining Build items were an `rdtscp` timer and an HDR-style histogram in place of the raw per-event vector. The hypotheses: rdtscp lowers the ~20-30 ns timer floor, and a 15 KB histogram disturbs the book's cache less than a 4 MB vector.

**What happened:** A log-linear histogram (exact < 64 ns, <= 1/32 above) now backs `ReplayStats::latency`, and its percentile rank matches the old `nth_element` definition. `TscTimer` (`rdtscp` + `lfence`, calibrated against `steady_clock`) is opt-in through `ReplayConfig::latency_timer` / `replay_cli --tsc`. Timer overhead is now printed next to every latency report.

**Problems and confusion:** The first version instantiated the replay loop once per timer. Cachegrind showed +27 instructions/event even with latency *off*: GCC had stopped inlining the map, flat-index and unlink calls. `cg_annotate` located it, and going back to one instantiation per book with a runtime timer switch fixed it. Also, on WSL2 the kernel clocksource is already the TSC, so `steady_clock` costs the same ~20 ns as `rdtscp`.

**Evidence and decision:** The histogram cut latency-on simulated LLd misses from 4.96 to 4.01/event (equal to unmeasured) and I refs by ~27/event, but wall-clock ranges overlap with the old vector. rdtscp lowered throughput by ~10% (lfence serialization) with the same p50. Keep the histogram, keep `steady_clock` as the default, and leave rdtscp opt-in. Details: `2026-09-27-phase4-tsc-histogram.md`.

**Left to revisit:** Batch timing (N events per sample) for sub-overhead resolution; rdtscp vs `steady_clock` on a native host, including a non-TSC clocksource; the Phase 4 Verify step (same benchmark on separate days, < ~5% variance).

**Ideas to test:** Repeat trade-enabled controlled depths on native Linux with branch/cache counters if available; vary configured trade rate and order size while recording actual trades and depth. After any replay-loop change, check the unmeasured-path instruction count, because measurement code can perturb inlining.

### Phase 4: Native Linux, Day 1 (2026-09-27)

**Idea:** Answer what WSL2 could not: a hardware counter behind each structural swap, THP vs small pages, the two timers on real hardware, TSan, and run-to-run variance. The baseline was a live USB on the same laptop, so results sit directly beside the WSL2 rows.

**What happened:** `scripts/native_run.sh full` ran twice, about an hour apart. The first run exposed two script problems: live-session detection relied on `boot=casper`, which 24.04 ISOs no longer pass, and TSan aborted under the kernel's ASLR entropy. Both were fixed in `f609efb` before the second run. All golden checksums matched, and they match WSL2's.

**Evidence and decision:**
- Every swap except the flat index now has a counter behind it.
  - Slab beats new/delete by ~100 fewer instructions and ~1.2 fewer cache misses per event.
  - Larger order records cost dTLB misses rather than LLC misses on the 5M log.
  - Hot/cold is slightly *worse*, with more cache misses per event.
  - Dense levels run more instructions but halve branch misses (IPC 1.33 vs 0.96), winning 14–23% where depth is bounded and nothing on the 5M log.
  - Sorted levels lose on instruction count.
- System-wide THP cut dTLB misses about 150× and gave +5–6% timed throughput on 5M.
- The native clocksource is also `tsc`, so rdtscp still has no advantage.
- Decision: keep the defaults. Tree + slab + 32B stays default. Dense stays experimental until a representative (ITCH-derived) workload shows the same bounded-depth advantage.

**Problems and confusion:** The second run's 5M throughput was 6–10% lower across every variant, with deep single-run dips at the same end temperature. Likely interference (candidates: the persistent stick's writes, or khugepaged after the THP toggle), not a change in the engine. The system-wide THP switch also affects everything else on the machine; a pool-level `MADV_HUGEPAGE` test would be cleaner.

**Left to revisit:** Days 2 and 3 of the variance check. A CLI path for `prefault_pool()` and pool `MADV_HUGEPAGE`. Counters for controlled-depth streams. A flat-index vs `unordered_map` counter comparison (build `6e1ea00`). Details: `2026-09-27-native-linux.md`.

## Phase 5: Replay Framework and Market Semantics

**Original idea:** Grow deterministic replay into a realistic event-driven harness, with stable serialization and strategy evaluation.

**What exists:** Synthetic events, replay, checksums, event-log support, multiple order-storage variants, and a CLI/benchmark path are already scaffolded.

**Decision to start (2026-09-27):** Move to Phase 5 instead of polishing Phases 2–4 further. The open questions from those phases (dense vs tree levels, hot/cold, huge pages) depend on the workload, and only real (ITCH-derived) data can settle them. Tuning on the synthetic generator risks optimizing for it.

### Phase 5: Golden File and Format Hardening (2026-09-27)

**What happened:** Committed `tests/data/golden_2000_seed7.bin` with its expected replay results (checksum, trades, volume, cancel misses, resting orders), checked on all 8 storage variants. A second test checks that the generator still reproduces the file byte-for-byte. Before this, the only guard was "generator output + engine → checksum", so a generator change and an engine bug looked identical. Now they fail different tests. The v1 file format is written down in `architecture.md`.

**Problems found:**
- `read_events` trusted the header count: a corrupt count meant an arbitrarily large `resize`, and trailing garbage was accepted. It now checks the count against the file size before allocating, validates enum and reserved bytes, and reports a reason.
- The new empty-log test failed under UBSan: `fwrite`/`fread` were being passed a null `data()` for zero records, which is undefined behaviour even with a zero count. Both calls are now guarded. This is the sanitizer build earning its keep on a path the Release build never flags.

**Next:** the `mmap` reader (5a), reusing `first_invalid_event` so both readers validate the same way.

### Phase 5(a): Memory-Mapped Reader (2026-09-27)

**What happened:** `MappedEventLog` (RAII, move-only) maps the file with `PROT_READ`, `MAP_PRIVATE | MAP_POPULATE` and exposes the records as a `span<const Event>` in place. Header checking moved into a shared `check_event_log_header`, so both readers enforce the same rules and produce the same error text (a test asserts this for every malformed case). `replay_cli` now maps by default. `--read` keeps the old path, and a `load` line reports the time.

**Decisions:**
- **Validate every record even when mapping.** This costs one sequential pass, but a zero-copy reader that trusts arbitrary bytes would let a bad enum byte reach the book. Because that pass touches every page anyway, `MAP_POPULATE` pre-faults the whole mapping in one go.
- **Keep `perf stat` comparable across days.** Counters include the load, so every existing perf variant in `native_run.sh` stays on `--read`, as on day 1. A separate `mmap` variant measures the reader change on its own.

**Evidence so far:** identical golden checksums from both readers on the 2k, 1M and 5M logs. WSL2 load of the 5M log, one informal run: 53 ms (mmap) vs 95 ms (read). Native counters are pending.

**Left to revisit:** Streaming replay for logs bigger than RAM (5b) will need windowed mappings, or `MADV_SEQUENTIAL` with `MADV_DONTNEED` behind the cursor, instead of one populated mapping.

### Phase 5(c): Modify and Reduce Events (2026-09-28)

**What happened:** `OrderBook::modify(id, price, qty)` and `reduce(id, by)`, with naive twins in the `RefBook` oracle, unit tests for each priority case, and a differential mix that includes both on 3 seeds and all 8 variants. A mutation test confirmed the harness notices a broken priority rule: letting a same-price qty increase keep its place fails both unit and differential tests. In the log format, `EventType` gains `Modify=2` and `Reduce=3` under magic `LOBEVT02`, and a second golden file (`golden_v2_2000_seed7.bin`, `gen_events … 2000 7 15`) is pinned on all variants and both readers.

**Decisions:**
- **Same-id Modify, 32 B record unchanged.** ITCH's replace (old ref → new ref) will be mapped onto the same engine id by the converter (5d). The alternative, a second id per record, would have grown every event to 40 B and invalidated all throughput baselines.
- **Exchange priority rule.** A lower qty at the same price keeps queue position. A price change or a qty increase re-queues the order, and it may trade, with itself as taker. `Reduce` (ITCH 'X', and executions later) keeps position. A modify is implemented as cancel + add, so the add can never be rejected for capacity.
- **v1 files stay v1.** `write_events` only writes `LOBEVT02` when a Modify/Reduce is present, and the generator draws no extra random numbers at `modify_percent = 0`. The committed golden file and `data/*.bin` are therefore byte-identical, and the generator-reproduction test didn't change.

**Problem found:** the first version raised instructions on v1 logs by 19% (Cachegrind, 1M, 234.9M → 279.4M) with unchanged checksums, and timing noise hid it. `cg_annotate` showed `add()`, `std::map::operator[]` and the flat-index calls out of line. That is the same inlining-budget failure Phase 4 hit. Two causes:
1. `modify`/`reduce` gave `add`/`cancel` a second call site in the same translation unit.
2. After a refactor, the sink became a named struct. `add<sink>` then got external linkage, and GCC's called-once inlining applies only to local functions.

Forcing `always_inline` on `add`/`cancel` only moved the problem to the leaf helpers (264.8M). The fix is structural: the Modify/Reduce dispatch lives in its own TU (`replay/src/amend.cpp`), and the hot loop keeps its local lambda sink. Result: 233.9M (−0.5% vs base).

**Lesson (5c):** "checksum unchanged and timing within noise" is not enough evidence that the hot path is unchanged. Compare instructions per event (Cachegrind or `perf stat`) after any change that adds a call site to an inlined function, not only after changes to the loop itself.

### Phase 5(f)+(g): Strategy Order Entry and Metrics (2026-09-28)

**What happened:** `replay_trading` lets a strategy trade against the replayed feed through an `OrderGateway` (submit/cancel/modify). Every action reaches the book after a configurable latency, and fills are reported back with exact remaining quantity. `StrategyMetrics` reports:
- counts: fills (and how many were aggressive), fill rate, and in-flight actions at the end;
- position and cash;
- realised PnL (average cost) and marked PnL;
- slippage against the mid at send time.

A deliberately simple `MarketMaker` (joins the touch, position limit) runs as `replay_cli --strategy mm`. Its golden result is pinned, and hand-computed scripted scenarios pin the accounting.

**Decisions:**
- **A separate replay loop, not a flag in `replay()`.** After 5(c), the cheapest way to keep the hot loop's instruction count unchanged is not to touch it. Plain-replay I refs moved 0.003%. A test proves the two loops agree: a strategy that sends nothing reproduces `replay()` exactly on both golden logs.
- **Latency is measured in feed time, not wall time.** An action sent at t is applied before the first feed event at or after t + latency. This keeps replays deterministic, and makes latency a parameter one can sweep: aggressive fills go 0 → 115 → 1,619 as latency goes 0 → 1 → 10 µs on the 1M log.
- **Fills arrive after the book operation completes.** A strategy reacting inside the match loop would see a half-matched book, and any order it sent would read a transient mid.
- **Reserved id range (≥ 2^63) for strategy orders.** Feed ids in that range are skipped, so the two can never collide.

**Problem found:** the market maker first tracked its orders' remaining quantity itself. A fill on the old quantity, landing between sending a modify and its arrival, made that count wrong, and it could underflow. The fix is `Fill::remaining`: the replayer, which sees the book, reports the exact number.

**Honest caveat:** the market maker "makes money" on the synthetic feed only because the generator has no informed flow. The metrics are verified; the strategy result means nothing until it runs on ITCH-derived data (5d).

### Phase 5(d): ITCH Converter and the First Real Data (2026-09-28)

**What happened:** a streaming parser for Nasdaq TotalView-ITCH 5.0 converts one symbol of a full trading day (2020-01-30, 12.95 GB, 423M messages) into a v2 event log in about 4.3 s. AAPL, AMD and TQQQ (2.0M, 2.4M and 3.4M events) rebuild exactly:
- 0 unknown references and 0 misses;
- 0 engine trades, because a displayed book never crosses, so any trade would have meant a mapping bug;
- identical mid-session checksums on all 8 storage variants.

The unit tests build ITCH messages byte by byte instead of committing a binary fixture.

**Decisions:**
- **The engine id is the original ITCH reference.** The converter maps later references (from U) back to it.
- **U follows Nasdaq's rule, not the engine's.** An ITCH replace always loses priority. The engine's same-price reduction keeps it, so in that case the converter emits Cancel + New instead of Modify.
- **Prices stay in ITCH units (1/10000 $).** This is lossless, but it matters for layouts (below).

**What real data showed immediately:**
- **The dense-vs-tree comparison was never general.** `DensePriceLevels` has a band hard-coded to the synthetic range (9,000–11,000), so on AAPL (about 3,235,250 in ITCH units) every level goes to the overflow map, and "dense" is 22–30% slower. The Phase 3 result was a property of the synthetic generator's price range. That is exactly the risk the Phase 5 start decision named.
- **Replaying ITCH gives strategies one-sided fills.** Executions reduce the named order, and the real aggressor never reaches the engine. So a strategy's resting order is filled only when a displayed Add crosses it, which means the market moved through the quote. The market maker marks about −$3.9k on AAPL, with 100% fill rate and favourable per-fill slippage. That is the signature of stale quotes being picked off, not a strategy result.

**Next questions:** a queue-position fill model for strategies on ITCH data; anchoring the dense band to the instrument, with real ticks, before re-running the level-store comparison; a market maker that excludes its own order from the touch.

### Follow-up: Dense Levels on Real Data (2026-09-28)

**What happened:** the dense band now centres on the first price it stores, and the converter can write cents (`--tick 100`). The first strict cents conversion failed on all three symbols: real sub-dollar orders are sub-penny (a $0.0001 AAPL bid). Dropping them is explicit and counted (`--drop-off-grid`: 11, 4 and 3 orders). With that in place, dense beats the tree by 14–22% on AAPL, AMD and TQQQ, and every dense run beats every tree run. Cachegrind attributes it to 16% fewer instructions and 41% fewer mispredictions; memory misses don't change.

**Lesson:** the Phase 3 question ("dense or tree?") had two wrong answers before this one:
- "dense wins" on synthetic data, where the band happened to fit the generator;
- "dense loses" on ITCH, where the band fitted nothing.

A layout's result is only as general as its assumptions about the data (price range, tick size). Validating on real data twice changed the conclusion, and both times the fix was about the data's units and range, not the data structure.

**Decision (user, 2026-09-28):** keep the tree as the default. Make dense the default once a band that re-centres itself has been measured, since the anchored band still sends 4–15% of prices to the overflow map.

### Phase 5(b): Streaming, and the Bug a Long Log Found (2026-09-28)

**What happened:** `EventLogStream` replays a log through a fixed window of records, so memory is the book plus the window whatever the file size: 69–102 MB vs 225 MB on the 5M log. Results match full-load replay exactly at any window, including a window of one event.

**Problem found:** the first 50M-event run never finished. Replaying prefixes showed a cliff: 15.5M ev/s at 10M events, 0.41M at 15M, and nothing within 150 s at 20M.
- **Cause:** the Phase 3(a) flat index hashed with `std::hash<uint64_t>`, which libstdc++ implements as the identity.
- **Mechanism:** sequential ids plus linear probing, with old orders still resting after the ids have wrapped the table, grow runs that "surf" forward. The table was only 37% full.
- **Why it was never seen:** every benchmark had stopped at 5M events.

**Fix and trade-off:**
- Scattering every id (Fibonacci hashing) removes the cliff but costs 43% at 1M. The identity's locality for recent ids had been doing real work in a 32 MB table.
- Keeping 64 sequential ids together and scattering the blocks recovers the misses; only hash arithmetic remains. That costs 12–17% on 1M–5M, and it's the fastest option from 10M to 50M.
- Bigger blocks bring the clustering back on long logs.

**Lesson (5b):** a benchmark range is part of the result. Every structure should be run at least once far beyond the size where its behaviour is supposed to change (here: many wraps of the table). The Phase 4 methodology never asked for that.

### Phase 5(e): Timestamp-Paced Replay (2026-09-28)

**What happened:** `replay_paced` applies each event at start + (ts − ts₀) ÷ speed and records how late it was against that schedule. Measuring against the schedule means a backlog shows up as growing lateness instead of being hidden (coordinated omission). Results are identical to plain replay.

**What it showed:**
1. The paced loop saturates at about 16M ev/s. Two clock reads per event cost it about 30% against plain replay.
2. At real-time speed the worst lateness came from WSL2 oversleeping in `sleep_until`, not from the engine. Faster speeds, which spin instead of sleeping, had better tails.
3. On AAPL, 1.8% of events are more than 1 ms late even at a 2% average load, because ITCH order flow arrives in microbursts. The synthetic generator's even spacing can never show this.

### Follow-up: Re-Centring Band, and Dense Becomes the Default (2026-09-28)

**What happened:** the dense band now rebuilds itself around the new best price whenever the touch leaves it. On AAPL, AMD and TQQQ that changes nothing, because their prices stayed within ±$10 of the pre-market price. So I added TSLA, which had its post-earnings session that day: only 2% of its prices were within ±$10 of the first one. There the anchored band was no better than the tree, and re-centring won 22% (−51% mispredicts). Per the user's decision, dense became the default: +14–27% over tree on all four real symbols.

**Problem found:** the first default build lost inlining again (376 vs 274 I/event against tree). As the default, the dense book now also compiles the strategy callback into the loop, and the rebuild path was inlined into `get_or_create`. Moving the outside-band path out of line fixed the real-data numbers. Synthetic data still shows +43% instructions (bitmap scans in a shallow book), but timing is within noise.

**Lesson:** "make X the default" is a hot-path change even when X was already measured as a variant: the default type is compiled with more code around it. Measure the default configuration itself, not the variant.

### Phase 5 Exit (2026-09-28)

**Criterion met.** Five logs of 2M–50M events (synthetic and real Nasdaq ITCH) were replayed end-to-end through every reader and the trading replay, twice. All 20 outputs match bit-for-bit apart from timing, and the readers agree on every log.

**What Phase 5 changed about earlier phases:**
1. Real data overturned the synthetic dense-vs-tree answer twice before settling it.
2. A 50M log exposed an index-hash collapse that every benchmark up to 5M had hidden.
3. Real order flow showed microbursts, and one-sided strategy fills, that the generator could never produce.

**Carried forward:**
- a queue-position fill model;
- a re-centring dense band, which then makes dense the default (user decision);
- a market maker that ignores its own order at the touch;
- native variance days 2–3.


**Still planned in the roadmap:** `mmap`-based zero-copy reading, streaming logs larger than RAM, replace/modify events, a public ITCH-subset parser, timestamp-paced replay, a richer strategy API, and PnL/fill/slippage metrics.

**Problems/decisions to capture as work begins:** Define event-format versioning, endianness, malformed-record behavior, timestamp ordering, and whether strategy submissions are synchronous or latency-simulated. Validate against a small committed golden log before scaling up.

**Ideas to revisit:** Use real public market data only after the internal event semantics and golden checksums are stable. Keep ingestion, matching, and strategy decisions separable for debugging.

### Follow-up: Queue-Position Fills (2026-09-28)

**What happened:** the ITCH converter now keeps executions (E/C) distinct from cancels (X), using a new Execute event in format v3; the engine treats it as a reduce, so no checksum changed. In the trading replay, each feed execution also fills the strategy orders that price-time priority puts ahead of the executed order, while the feed book stays exact.

**Result:** the market maker's fills become two-sided and 1.5–2× more frequent on AAPL, AMD and TSLA, but it still loses about 1.35 ¢/share on AAPL.

**What the result says:** a better fill model exposes the strategy instead of excusing it. The market maker reads its own lone order as the touch, keeps quoting a price the market has left, and gets picked off when the market trades through. The next step is the strategy (follow-up 3), not the simulator.

### Follow-up: The Market Maker Stops Following Itself (2026-09-28)

**What happened:** the market maker now quotes at the best price held by others. When its own order is alone at the best level, it moves to the next one, using a new `next_price_after` query implemented on all three level stores and property-tested against the tree. On the four real symbols, losses fall 4–60× and filled volume falls 3–13×: the removed fills were exactly the ones where the market traded through a price only the market maker still quoted.

**What is left:** a quoter with no signal that re-prices constantly. Results are near zero and inconsistent in sign across latency settings (TSLA +$281 at 0 µs, −$2,618 at 10 µs). That is noise, and the docs say so.

**Lesson:** the chain of fixes (fill model, then strategy) only worked in that order. With one-sided fills the strategy flaw was invisible behind the simulator's flaw. With a fair simulator, the strategy flaw had nowhere to hide.

## Phase 6: Concurrency (Optional Stretch)

**Original idea:** Test a reader-to-engine SPSC queue and study cache-coherence costs without parallelizing the matching core itself.

**Status:** Not started; optional stretch, not a prerequisite for a useful single-threaded engine.

**Problems/decisions to capture later:** Establish a measured bottleneck first. Define ownership and shutdown behavior, memory-order requirements, backpressure, and what happens when the queue is full. Compare against a simple mutex queue and retain deterministic checksums.

**Ideas to test:** Add `alignas(64)` only when measuring false sharing; use TSan on a host where its runtime is supported. Avoid adding concurrency merely to increase thread count.

## Phase 7: Hardening and Polish

### Phase 7: Invariant Checker, Fuzzer, clang-tidy, README (2026-09-28)

**What happened:**
- `validate()` checks the book's structural invariants and reports the first violation with its location.
- A libFuzzer harness drives the dense and tree books and the naive oracle with the same generated operation streams, and calls `validate()` after every operation. 21.2M operations ran clean under ASan/UBSan.
- clang-tidy, with warnings as errors, now runs in CI. It found two unchecked `rewind` calls in the log readers.
- The README was rewritten around the architecture, the measured results (with provenance) and commands that were each re-run before committing.

**How I know the tools work:** a deliberately injected bug in each. A wrong level total was reported by `validate()` 980 times with its location. A priority bug was found by the fuzzer from an empty corpus in about 2,500 operations.

**Lesson:** a checker that has never caught anything proves nothing. Inject one bug per checker before trusting it: that is the Phase 0 habit, applied again.


**Original idea:** Stress the engine, document invariants and architecture, and make the benchmark reproducible for another developer.

**Status:** Not started as a final phase. Some pieces, including sanitizers, tests, and architecture notes, are already used throughout development; final hardening still remains.

**Problems/decisions to capture later:** Fuzz operation sequences and preserve minimized failures; define overflow/exhaustion behavior; document supported price/ID/capacity bounds; check API behavior under pool exhaustion and invalid input.

**Ideas to test:** Add long deterministic fuzz runs under ASan/UBSan, a `validate()` consistency walk, a concise architecture diagram, and reproducible setup/benchmark instructions.

## Cross-Phase Rules of Thumb

- Correctness evidence comes before performance claims: preserve differential tests and replay checksums after structural changes.
- Change one major representation at a time so benchmark deltas have an interpretable cause.
- Keep hypotheses, measured results, and conclusions labeled separately.
- If a tool or host cannot provide a measurement, record the limitation and the next environment in which to revisit it.
- Deferred work is not forgotten work: state its trigger, required evidence, and whether it blocks progress.
- Keep local milestone commits small and do not push to a remote unless explicitly requested.

## How to Update This Journal

For each phase or sub-phase, add a dated subsection answering:

1. What was the initial idea or hypothesis?
2. What did I implement or test, and what was deliberately left untouched?
3. What failed, surprised me, or was confusing?
4. What decision did I make, and what evidence supports it?
5. What did I defer, and what condition should trigger revisiting it?
6. What new idea or question emerged?

Link detailed experiment notes and benchmark rows instead of copying raw output. When a hypothesis changes, preserve the earlier reasoning and add the new decision with its date rather than silently rewriting history.
