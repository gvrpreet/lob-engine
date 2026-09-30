# Experiment: Phase 5(e) timestamp-paced replay            Date: 2026-09-28      Commit: f958d4c
**Hypothesis:**
- Replaying events at the pace of their timestamps (wall time = log time ÷ speed) changes when events are applied, never their order, so results are bit-identical to `replay()`.
- The useful output is *lateness*: how far behind its scheduled wall time each event was applied. It is measured against the schedule, not from the previous event, so a backlog is not hidden (coordinated omission).
- While the engine keeps up, lateness is bounded by wake-up precision. Once the event rate exceeds the engine's throughput, lateness grows without bound.

**Change:** `replay/paced.hpp` / `replay/src/paced.cpp`: `replay_paced(events, cfg, speed, strategy)`, a separate loop and TU on the default book, and `replay_cli --paced <speed>`.
- Waiting: `sleep_until` for gaps longer than 200 µs (waking 100 µs early), then spinning on `steady_clock`.
- Lateness goes into a `LatencyHistogram`, plus a count of events more than 1 ms late.

**Method:** WSL2, Release preset, `taskset -c 2`, `--no-strategy`, one run per row. Tests: golden v1/v2 at speed 10^12 against `replay()` (all stats identical); a hand-made 40 ms log at speed 1 and 2 (wall ≥ span ÷ speed, never early).

**Results:**

| Log | Speed | Demanded rate | Wall (span) | Lateness p50 / p99 / p99.9 / max | > 1 ms late |
|---|---|---|---|---|---|
| 1M synthetic | 1 | 1M ev/s | 0.999 s (0.999 s) | 8 ns / 30 µs / 7.6 ms / 8.4 ms | 8,170 |
| 1M synthetic | 10 | 10M ev/s | 0.100 s | 22 ns / 156 µs / 254 µs / 257 µs | 0 |
| 1M synthetic | 20 | 20M ev/s | 0.068 s (due 0.050) | 5.6 ms / 17.8 ms / 18.1 ms / 18.1 ms | 889,947 |
| 1M synthetic | 100 | 100M ev/s | 0.063 s (due 0.010) | 21.5 ms / 52.4 ms / 53.1 ms / 53.1 ms | 975,469 |
| AAPL cents, full day | 10,000 | about 346k ev/s on average, far more in bursts | 5.760 s (57,599 s) | 303 ns / 1.87 ms / 2.88 ms / 4.91 ms | 36,500 |
| AAPL cents, full day | 100,000 | | 0.576 s | 344 µs / 5.77 ms / 5.81 ms / 5.81 ms | 823,873 |

Checksums matched plain replay in every row (`ba7911b3a1dd1d8a`, `14650fb0739d0383`). Plain replay in the same session: 1M at 23.4M ev/s, AAPL at 15.9M ev/s.

**Explanation:**
- **The backlog appears where predicted.** Under saturation the paced loop runs at about 1M ÷ 0.063 s ≈ 16M ev/s, below plain replay's 23M. The difference is its own per-event cost: two `steady_clock` reads (about 20 ns each on this host) and a histogram record. Speed 20 asks for 20M ev/s, so lateness grows through the run (p50 5.6 ms). At speed 100, lateness approaches wall time minus span ÷ speed.
- **At speed 1 the median is fine, but the tail is worse than at speed 10.** Gaps longer than 200 µs go through `sleep_until`, and WSL2 sometimes oversleeps by milliseconds; the loop then catches up in a burst. At speed 10 almost every gap is below the threshold, so it spins and the tail stays under 260 µs. The tail comes from the wait mechanism, not the engine.
- **Real data is bursty.** AAPL at 10,000× averages only about 346k events/s of wall time (2% of the engine's rate), yet 1.8% of events are more than 1 ms late. ITCH has microbursts (many events within a few µs of log time), and compressed 10,000×, those exceed the engine's rate for a moment. That is the effect pacing exists to show, and the synthetic generator's even spacing (500–1,500 ns) cannot produce it.

**Surprises / next questions:**
- To make the tail depend on the engine rather than the OS: spin for all gaps under a few ms (costs a core), or use `clock_nanosleep` with `TIMER_ABSTIME` and a larger early-wake margin. Measure on the native host, where WSL2's timer slack doesn't apply.
- Lateness per burst on ITCH data could be reported against burst size, a first look at a queueing model. It would also give the strategy latency in 5(f) a real-time meaning.
