# Experiment: Phase 7 hardening (invariant checker, fuzzing, clang-tidy, README)            Date: 2026-09-28      Commits: b0d18ee, baf4791 (+ README/docs)
**Hypothesis:** the differential test and the golden files check *results*. An invariant checker and a fuzzer that search for inputs, rather than replaying fixed seeds, should catch bugs that fixed seeds miss. Neither should cost anything on the hot path.

**Change:**
- `OrderBook::validate(std::string* why)`, a read-only O(orders + levels) check. It reports the first violation, and implements the state invariants of `architecture.md` §4:
  - (1) every resting order is in exactly one level list, once, and the id index maps it to its handle;
  - (2) the book is never crossed;
  - (3) level `total_qty`/`count` match the list, and no empty level is stored;
  - (6) the pool's live count equals the listed orders.

  It also checks prev/next/tail links, price-priority order of the levels, and each order's price and side against its level. Called from the differential tests every 512 ops, from the drift test every 256, from a unit test after every operation kind, and from the fuzzer after every operation. It is a template member, so it is only compiled where called: v1 1M I refs stay at 361.5M.
- `tests/fuzz/fuzz_book.cpp` (libFuzzer + ASan + UBSan, clang, `-DLOB_FUZZ=ON`). The input is read as a stream of 3-byte operations: limit/market add, cancel, modify, reduce, and market jumps of up to ±127k ticks that force dense re-centring. Each operation runs on the dense (default) book, the tree book and the naive `RefBook` oracle. Results, trades, order counts and best prices must match, and `validate()` must pass. `scripts/fuzz.sh [seconds] [jobs]` keeps a corpus and reports the operations checked.
- `.clang-tidy` (bugprone-*, performance-*, clang-analyzer-*, warnings as errors) and `scripts/tidy.sh` over all 15 library, tool and bench sources. CI gains `tidy` and a 60 s `fuzz` job next to the g++/clang × debug/asan/release matrix.
- README rewritten: architecture diagram, results table (with provenance), and reproducible commands. `phase5_exit.sh` and `stream_measure.sh` moved from the git-ignored `results/` into `scripts/`.

**Results:**
- **Mutation checks:**
  - `set_resting_qty` forgetting the level total: `validate()` failed 980 times in the differential suite, with the exact location (`bid level 91: total_qty/count disagree with the list`).
  - Modify keeping priority on a same-price quantity increase: the fuzzer found it from an empty corpus after 2,467 operations (`modify result differs`).
- **Fuzz run:** 6 jobs × 330 s on WSL2 = **21,201,829 operations** checked against the oracle, clean under ASan/UBSan, no crash, corpus of 2,502 inputs. Throughput is about 5.7k operations/s per job; the O(n) oracle and a `validate()` after every operation dominate.
- **clang-tidy:** 2 real findings, both `bugprone-unsafe-functions`: `rewind` has no error detection, used in `read_events` and `EventLogStream::open`. Replaced by a checked `fseek`. Clean afterwards. A deliberate violation makes `tidy.sh` exit 1.
- **Tests:** `check.sh` passes (94 tests). Every README command was smoke-tested; regenerated synthetic and ITCH logs are byte-identical.

**Explanation:** the fuzzer is valuable because it explores operation sequences that fixed seeds never produce: modifies of ids that were just filled, prices jumping across the dense band, zero quantities. `validate()` turns silent structural corruption (a wrong level total that only matters much later) into an immediate, located failure.

**Surprises / next questions:**
- The Phase 7 "property test: sum of maker fills == sum of taker fills" is implied by the oracle comparison: every trade is compared field by field with an independent implementation. A separate property test would add little.
- Still open as extensions: IOC/FOK/post-only order types. Running `validate()` under a debug-build flag in the replay loops, not only in tests, would catch corruption on real logs too.
- The O(n) oracle limits the fuzzer to small books (up to about 1,365 operations per input). Deep-book bugs need the drift and differential tests, or a faster oracle.
