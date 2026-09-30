# Experiment: Phase 5(b) streaming replay for logs bigger than memory            Date: 2026-09-28      Commit: abca180 (index hash fix b47f506 first)
**Hypothesis:**
- Replaying through a fixed window of records gives results bit-identical to the full-load path at any window size, down to one event. The window only changes when records are read, not what the book sees.
- Resident memory is then bounded by the book plus the window (tens of MB) instead of growing with the log. The copying reader holds the whole log (a 50M-event log is 1.6 GB). The mmap reader maps it all: page-cache pages are reclaimable, but they still count towards RSS.
- Throughput stays within about 10% of the mmap path. Sequential `read` into a 32 MB window is cheap next to matching.

**Change:**
- `replay/include/replay/event_stream.hpp` / `replay/src/event_stream.cpp`: `EventLogStream`, a `read`-based windowed reader. It checks the header against the file size up front, then validates each chunk before handing it out, using the same helpers and error text as the other readers, with absolute record indices.
- `replay_stream()`: its own loop in that TU. The plain `replay()` loop is untouched.
- `replay_cli --stream [--window N]`.

**Method:** tests at windows of 1, 7, 2,000 and bigger than the file, on golden v1 and v2, against `replay()`; malformed-record and truncated-file cases. A synthetic 50M-event log (1.6 GB) and the 5M log: `/usr/bin/time` max RSS and throughput for mmap, `--read` and `--stream`. Cachegrind I refs for plain replay (v1 1M) to confirm the hot loop is unchanged.

**Results:**
- Tests: `check.sh` passes (89 tests). Streamed replay matches `replay()` exactly (checksum, trades, volume, all miss counts, resting orders) on golden v1 and v2, at windows of 1, 7, 2,000 and 2^20; the VWAP strategy result is identical too.
- A bad record at index 1,234 with a 100-event window stops the replay after 1,200 events (12 whole chunks), with the same message as `read_events`. A truncated file fails at `open`, and an empty log replays 0 events.
- 5M synthetic log (pre-hash-fix build `cfbb255` + stream code, 3 runs each, `--no-strategy`, `taskset -c 2`); checksum `01e9c2940a0e996f` everywhere:

| Reader | ev/s | Max RSS |
|---|---|---|
| mmap (default) | 19.91 / 20.20 / 20.28M, plus 44 ms load | 225 MB |
| `--read` | 18.28 / 15.74 / 19.42M, plus 80–85 ms load | 225 MB |
| `--stream` (window 2^20 = 33.6 MB) | 14.47 / 17.62 / 16.13M, reads included | **102 MB** |
| `--stream --window 65536` | 16.03 / 17.71 / 18.23M | **71 MB** |
| `--stream --window 1024` | 17.55 / 16.76 / 17.54M | **69 MB** |

- 50M synthetic log (1.6 GB): the first attempt exposed the index-hash collapse (see `2026-09-28-index-hash-clustering.md`).
- After the fix, the user ran `scripts/stream_measure.sh` on commit `9b37878` (log `results/stream_measure_20260928_082736.log`): 3 runs per reader, `--no-strategy`, `taskset -c 2`. Median ev/s / max RSS:

| Reader | 5M | 50M |
|---|---|---|
| mmap (default; excludes load time) | 18.56M / 219 MB | 13.72M / **1,593 MB** |
| `--read` (excludes load time) | 18.47M / 219 MB | 13.28M / **1,593 MB** |
| `--stream` (window 2^20) | 16.07M / 99 MB | 11.68M / **99 MB** |
| `--stream --window 65536` | 15.27M / 69 MB | 12.23M / **69 MB** |
| `--stream --window 1024` | 16.41M / 67 MB | 11.90M / **67 MB** |

  Checksums were identical in every run: 5M `01e9c2940a0e996f`, 50M `e45d178766abc830`. The 50M log saturates the 1M-order pool (1,048,576 resting, 8.36M rejected adds).

**Explanation:**
- **Memory behaves as predicted.** Streaming holds the book, about 66 MB, plus the window: 67–99 MB whether the log is 5M or 50M events. mmap and `--read` hold the book plus the whole file: 219 MB at 5M and 1,593 MB at 50M. The mmap pages are reclaimable page cache, but they count towards RSS and grow with the log. At 50M, streaming uses 94–96% less memory.
- **Throughput costs 11–15% at 50M** (11.68–12.23M vs 13.72M ev/s), slightly beyond the predicted ~10%. The comparison favours mmap, because its figure excludes load time (tens of ms at 5M), while streaming's includes every `read`.
- **Window size barely matters:** 1k, 64k and 1M windows are within noise of each other. Sequential `read` into a warm buffer is cheap next to matching, so the smallest window that amortises the syscall is enough.

**Surprises / next questions:**
- The 50M run found a real bug unrelated to streaming: the index-hash collapse after about 10M events.
- The 50M synthetic log saturates the 1M-order pool, so a larger `book_capacity` (a CLI flag?) is needed for honest 50M+ numbers.
- Streaming is default-book only, with no latency measurement. Extend it only if a use needs it.
