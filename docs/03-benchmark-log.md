# Benchmark log (append-only)

Never edit old rows; add a new one. Numbers below are placeholders for **your** machine.
Hardware: `<CPU model, cores, SMT on/off, RAM>` · OS/kernel: `<...>` · Compiler: `<g++ 13.x / clang 17>` · Flags: `<...>`

| Date | Commit/tag | Change | Workload (events, seed) | ev/s median (min–max) | p50 / p99 / p99.9 ns | IPC | LLC-miss/event | Checksum | Notes |
|---|---|---|---|---|---|---|---|---|---|
| YYYY-MM-DD | phase1-baseline | std::map + unordered_map + slab | 5M, seed 42 | _fill_ | _fill_ | _fill_ | _fill_ | _fill_ | baseline |
|  |  | flat id index |  |  |  |  |  |  |  |
|  |  | tick ladder + bitmap |  |  |  |  |  |  |  |
