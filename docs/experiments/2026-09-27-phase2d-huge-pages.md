# Experiment: pool + index on huge pages (phase 2d)            Date: 2026-09-27      Commit: baseline 6df2da9 + working tree
**Hypothesis:** The native system-wide THP run cut dTLB misses on the 5M log about 150× and raised timed throughput by 5–6%. That run also put the 160 MB event buffer on huge pages, so it could not say where the gain came from. The book's random-access structures are the 32 MiB order pool and the 32 MiB id index. Only those two should matter for the timed loop: the event buffer is read sequentially, costing about one TLB miss per 4 KB (≈40k for 5M events, against 1.5M total). Prediction: huge pages for the pool and index alone recover most of the timed-loop gain and the dTLB drop, but only a small share of the page faults, since most faults come from loading the event file.
**Change:**
- `lob/page_allocator.hpp`: `HugePageAllocator` gives 2 MiB-aligned storage (`aligned_alloc`), rounded up to whole huge pages, with `madvise(MADV_HUGEPAGE)` applied before the container first touches it.
- `BasicOrderPool` and `BasicFlatOrderIndex` take the allocator as a template-template parameter, defaulting to `std::allocator`.
- `HugePageOrderBook` = the default tree book with both structures on the new allocator.
- `replay_hugepages()` also records the process's `AnonHugePages` from `/proc/self/smaps_rollup`.
- Exposed as `replay_cli --huge-pages` (a storage variant), a `32B huge` row in `bench_match`, and a `hugepages` `perf stat` variant in `native_run.sh full`.

An earlier draft used a runtime flag inside a stateful allocator. It was rejected because it added ~1.7 instructions per event to the *default* path (Cachegrind: 254.90M → 257.70M on the 1M log): changing the default container type changes its code generation. With the type-based variant the default path is unchanged (254.90M → 254.91M).
**Method:** WSL2 functional check only (the THP mode there is `madvise`, and hardware counters are unavailable). Native timing and counters come on the next native day.
**Results (WSL2):**
- Checksums match golden (1M `ba7911b3a1dd1d8a`, 5M `01e9c2940a0e996f`). New tests cover allocator alignment, value initialization and reallocation, book equivalence, and replay equivalence.
- `AnonHugePages` = 65,536 kB at the end of replay: exactly the 32 MiB pool plus the 32 MiB index, so the hint is honoured.
- 5M minor page faults: 55,600 → 39,253. That is −16,347, which matches 64 MiB of 4 KB pages. The remaining ~39k are mostly the event buffer.
- WSL2 5M timing, 5 alternating runs: default 17.92–19.53 vs huge 18.43–20.05 M ev/s. The ranges overlap, which is expected on this host.
- Cachegrind on the 1M log: the huge variant runs 1.07M extra setup instructions (the custom allocator loses libstdc++'s trivial fill path) plus ~1.65 instructions per event from different code generation. That is a small bias *against* huge pages.

**Explanation:** Pending native counters. What the design already shows: the default slab is fully faulted in by its constructor (the free-list loop writes every slot), so the old `prefault()` is a no-op for it, and "prefault-only" is simply the default. The real comparison is the default vs pool/index on huge pages vs system-wide THP.
**Surprises / next questions:**
- The runtime-flag draft repeated the lesson from the latency-histogram change: measurement or experiment plumbing on the default types perturbs their code. Check the default path's instruction count every time.
- On the native day, compare dTLB-load-misses and timed ev/s for `hugepages` vs `order32` vs the THP-`always` run:
  - If `hugepages` recovers most of the 5–6%, the book structures are the TLB-bound part.
  - If not, the sequential event stream matters more than predicted.
