# Changelog

Milestones are git tags. Numbers are summarised here; full provenance (commit, hardware, flags,
run count) is in `docs/benchmark-log.md`.

## 0.9.0 (2026-09-28): repository layout
- Component layout: `engine/` (`lob::engine`), `replay/` (`lob::replay`), `apps/`, `bench/`, `tests/{unit,fuzz,support,data}`.
- CMake options (`LOB_BUILD_TESTS/APPS/BENCH`, `LOB_INSTALL`) and `find_package(lob_engine)` support.
- MIT license, `.gitattributes`, `.editorconfig`, and docs reorganised (design docs, benchmark log, `guides/`).

## Strategy and fill model (2026-09-28): tags `queue-fills`, `mm-others-touch`
- Queue-position fills: ITCH executions become an `Execute` event (format v3). Feed executions fill the strategy orders that price-time priority puts ahead of them.
- The market maker quotes at the others' touch; losses on four ITCH symbols fall 4–60×.

## Phase 7: hardening (2026-09-28): tag `phase7-hardening`
- `OrderBook::validate()` (state invariants), a libFuzzer harness against a naive oracle (21.2M operations clean under ASan/UBSan), and clang-tidy in CI.

## Dense levels become the default (2026-09-28): tags `dense-itch`, `dense-default`
- The dense bitmap band re-centres on the touch; it is 14–27% faster than `std::map` levels on real Nasdaq flow.

## Phase 5: replay framework (2026-09-27/28): tags `phase5c-modify` … `phase5-complete`
- Golden files and a validated log format; mmap reader; Modify/Reduce (format v2); ITCH 5.0 converter; strategy order entry and metrics; streaming and paced replay.
- Fixed a flat-index hash collapse after about 10M events.

## Phases 0–4 (2026-09-23/27): tags `phase0-done`, `phase1-complete`
- Matching engine with slab pool and intrusive FIFO levels; allocator, record-size and hot/cold experiments; flat id index; dense and sorted level stores; log-bucket latency histogram and an `rdtscp` timer; native Linux `perf` runs.
