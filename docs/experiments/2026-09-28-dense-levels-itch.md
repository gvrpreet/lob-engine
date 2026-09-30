# Experiment: dense vs tree vs sorted levels on real ITCH order flow            Date: 2026-09-28      Commit: 214f281
**Hypothesis:** The Phase 5(d) result (dense 22–30% slower than tree on ITCH) is an artefact, not a verdict, for two reasons:
1. The dense band was hard-coded to 9,000–11,000, so every real level overflowed into the `std::map`.
2. In ITCH units (1/10000 $), a penny stock only ever uses every 100th slot.

With the band centred on the instrument and prices in cents, dense should beat the tree on real flow for the same reason as on synthetic data: fewer instructions and fewer branch mispredictions, with no tree descent per level lookup.

**Change:**
- `DensePriceLevels` without an explicit base now centres its 2,001-slot band on the first price it stores, clamped to `Price`'s range. `DensePriceLevels(base, span)` still pins the band, and the older unit tests use that to keep their coverage.
- `itch_convert --tick 100` writes cents. The default is strict: a price that isn't a multiple of the tick fails the conversion, naming the price. `--drop-off-grid` drops such orders instead, with their later messages, and counts them.
- The default tree book is untouched: v1 1M I refs are 233,849,657 (was 233,850,376).

**Method:**
- AAPL, AMD and TQQQ from the 2020-01-30 Nasdaq ITCH file, full day, in ITCH units and in cents.
- `replay_cli --no-strategy [--dense|--sorted]`, 7 runs each, `taskset -c 2`, WSL2, Release preset.
- Cachegrind (non-native Release, `--branch-sim=yes`) on the AAPL cents log.

**Results:**
- Strict cents conversion fails on all three symbols because of real sub-penny prices. Drop mode removes 11 / 4 / 3 orders, every one priced below $1 (mostly $0.0001 bids). Reg NMS 612 only requires penny increments at or above $1.
- 85.3% / 94.4% / 96.3% of New/Modify prices fall within ±1,000 ticks of the first price. Price ranges run from 1 to 19,999,999 cents, because of stub quotes.
- Mid-session cuts (first 1M events) in cents give identical checksums on all 8 variants and both readers: AAPL `c9917ec94f85c69d` (24,616 resting), AMD `3af832aa3a10512c` (11,042), TQQQ `3299659deadc2495` (3,786). 0 trades.

Throughput, full day, M events/s, median of 7 (min–max):

| Log | Tree | Dense | Sorted |
|---|---|---|---|
| AAPL cents | 16.63 (14.55–16.84) | **20.21 (20.02–20.73), +21.5%** | 14.47 (14.36–14.59) |
| AMD cents | 25.32 (25.18–26.16) | **28.98 (26.93–29.54), +14.5%** | 24.12 (23.88–24.32) |
| TQQQ cents | 19.69 (19.27–20.54) | **23.30 (22.68–23.89), +18.3%** | 19.64 (19.46–19.88) |
| AAPL ITCH units | 16.84 (16.55–17.18) | 12.90 (12.09–13.02) | 14.89 (14.59–15.17) |
| AMD ITCH units | 24.89 (23.86–25.85) | 18.85 (17.30–19.69) | 25.01 (24.67–25.35) |
| TQQQ ITCH units | 19.69 (19.47–19.76) | 14.44 (14.04–14.67) | 19.52 (18.27–19.76) |

Cachegrind, AAPL cents, full day (1,993,665 events), per event:

| Store | I refs | Branches | Mispredicts | D1 misses | LLd misses |
|---|---|---|---|---|---|
| Tree | 385.6 | 78.1 | 5.26 | 3.22 | 2.36 |
| Dense | 323.9 (−16%) | 62.0 | 3.10 (−41%) | 2.67 | 2.34 |
| Sorted | 1463.3 (×3.8) | 1050.5 | 5.14 | 12.72 | 2.34 |

**Explanation:**
- **In cents, dense wins on all three symbols, and every dense run beats every tree run.** The counters explain it: 16% fewer instructions and 41% fewer mispredictions (no red-black tree descent or rebalancing per level operation). Last-level misses are unchanged. This is the same mechanism the native `perf` run found on synthetic data (branch misses halved, IPC 0.96 → 1.33), now on real flow.
- **In ITCH units, the anchored band spans only ±$0.10**, so most levels still overflow. Dense then pays for its band check on top of the map, which is the 5(d) result again.
- **Sorted executes about 3.8× the instructions.** The real book is wide: tens of thousands of resting orders spread over many levels, including stub quotes far away. So vector insert/erase shifts many entries. Those shifts are predictable `memmove`s, which is why the time cost is only 0–13%.

**Surprises / next questions:**
- The band is anchored on the *first* price seen. For AAPL, 15% of prices still fall outside ±$10 of the 04:00 price. A band that re-centres (or a two-level bitmap over the whole range) might add more; measure the overflow share per day before building it.
- Should the default book switch to dense? The evidence now holds on synthetic data (native `perf`) and on three real symbols, but it depends on setup: cents ticks, and a band anchored on a sensible first price. **Decision (user, 2026-09-28):** keep the tree as the default, and switch to dense once a re-centring band has been measured.
- Sub-penny orders are real. Any tick-based layout needs a policy for them: overflow in the engine, or drop in the converter, as here.
