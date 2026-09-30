# Experiment: Controlled active-level depth            Date: 2026-09-26      Commit: baseline b30027a + working tree
**Hypothesis:** Holding the number of active price levels at 10, 25, or 50 will make tree/dense/sorted comparisons representative of shallow books and quantify how level count affects their relative throughput.
**Change:** Added deterministic `generate_depth_events(count, active_levels, seed)` with one persistent anchor order per requested level. Subsequent adds/cancels stay at those prices. `bench_match` accepts an optional second argument for requested levels; omitting it preserves the original generator. The benchmark reports requested, final, and peak levels; shape is collected in a separate untimed replay pass.
**Method:** For each depth, ran `taskset -c 2 build/release/bench_match 1000000 <depth>` three times. Each invocation performs seven alternating runs over all seven book variants. GCC 13.3, Release preset (`RelWithDebInfo`, `LOB_NATIVE=ON`), AMD Ryzen 7 7840HS, Ubuntu 24.04 under WSL2 kernel `6.18.33.2-microsoft-standard-WSL2`; hardware counters unavailable. This workload uses limit adds and cancels only, deliberately avoids trades, and keeps one anchor order per price level. It isolates level lookup/update cost but is not a full matching-workload comparison.
**Results:** Every invocation reported exactly the requested/final/peak level count and the same checksum per depth: 10 levels `696de5d2d747bf6f`, 25 levels `81dfcc26659fbde6`, 50 levels `30c25a6d88c9bb68`.

| Levels | Tree median range | Dense median range | Sorted median range | Aggregate min/max ranges (tree / dense / sorted) |
|---:|---:|---:|---:|---|
| 10 | 19.16-19.81M | 19.08-19.73M | 17.66-18.35M | 18.55-21.26M / 17.78-20.47M / 15.39-18.89M |
| 25 | 17.63-19.25M | 18.12-20.07M | 16.28-17.35M | 16.82-19.74M / 17.66-20.24M / 15.45-17.83M |
| 50 | 17.35-17.89M | 18.81-19.32M | 15.71-16.27M | 16.54-18.12M / 15.36-19.95M / 11.33-16.91M |

Throughput values are millions of events/s; median range is across three seven-run invocations. Min/max ranges overlap at every depth. The sorted vector was lower by median in all samples, but timing alone is not conclusive; dense's 50-level samples had especially wide variance. Checksums matched across all seven variants per invocation.
**Explanation:** The sorted-vector implementation did not show a benefit even at these controlled depths; vector insertion/erase movement may dominate. Dense levels were comparable at 10/25 and had higher medians at 50, but the ranges and especially dense's spread do not establish a stable win. WSL2 has no hardware counters to explain the differences. Keep the tree backend as default.
**Surprises / next questions:** The follow-up now adds market flow and preserves exact depth; see the trade-enabled note. Next, vary trade rate/order size and compare on native Linux with branch/cache counters if available.
**Trade-enabled follow-up:** See `2026-09-26-phase4-trade-depth.md` for 25%-configured market-flow runs at the same target depths. The follow-up preserves exact level counts and produces roughly 200k trades per 1M events.
