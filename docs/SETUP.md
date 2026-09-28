# Setup & Usage Manual (Linux / WSL2)

This is the on-ramp and the reference manual. Read §1–§5 once and run the commands. Come back to
§6–§8 whenever you need a tool's arguments or the expected outputs. For the learning/build plan,
see `docs/00-roadmap.md`; for the per-change loop, `docs/02-workflow.md`; for where the project
currently stands, `docs/last-task-completed.md`.

| Doc | Use it for |
|---|---|
| `SETUP.md` (this file) | install, build, verify, tool usage, troubleshooting |
| `00-roadmap.md` | phases, exit criteria, what to learn/build next |
| `01-architecture.md` | data model, invariants, design decisions |
| `02-workflow.md` | the 8-step change loop, definition of done, benchmark protocol |
| `03-benchmark-log.md` | append-only measured results |
| `last-task-completed.md` | current checkpoint, next task, native Linux checklist + WSL2 reference data |
| `NATIVE-LIVE-USB.md` | step-by-step native run from a live USB on the same laptop, with automatic result logging |
| `development-journey.md` | why decisions were made; what was deferred |
| `learning/experiments/`, `learning/notes/` | per-experiment records and daily notes |

---
## 1. Pick an environment

| Environment | Build + tests + sanitizers | Relative timing (A vs B, same run) | Absolute latency claims | `perf` counters | TSan | Huge pages / governor |
|---|---|---|---|---|---|---|
| **Native Linux** (bare metal) | yes | yes | yes, with §7 hygiene | yes | yes | yes |
| **WSL2** (Windows) | yes (ASan/UBSan) | yes, noisy across sessions | no | **no** | **builds, runtime unsupported** | no governor; THP exists but no TLB counters |
| Cloud VM / container | yes | yes | no | often partial or none | usually | usually no |
| macOS / native Windows | not supported: scripts need bash + `taskset`; untested | — | — | — | — | — |

On WSL2 you can develop, test and compare variants, but hardware counters, TSan and trustworthy
cross-day numbers need a native Linux host. See the "Native Linux Checklist" in
`last-task-completed.md`.

## 2. What to install

### Required
| Package | Why | Minimum |
|---|---|---|
| C++20 compiler: `g++` | builds everything | GCC 11 (tested: 13.3) |
| `cmake` | presets and build | 3.20 (tested: 3.28) |
| `git` | version control, commit hashes in benchmark rows | any |
| `build-essential` (make, libc headers) | presets use the **Unix Makefiles** generator | — |
| `util-linux` (`taskset`) | core pinning in `scripts/bench.sh`; preinstalled on Ubuntu | — |

### Recommended
| Package | Why |
|---|---|
| `clang` (≥ 14; tested 18) | CI builds with both `g++` and `clang++`; reproduce CI failures locally |
| `valgrind` | `scripts/perf.sh cache` (Cachegrind), the **only** cache-counter substitute on WSL2/VMs |
| `gdb` | debugging the `debug` preset |
| `clang-format` | the repo has `.clang-format` (Google style, 100 cols) |
| `clangd` (editor) | uses `compile_commands.json`; see §4 |

### Native Linux only
| Package | Why |
|---|---|
| `linux-tools-common linux-tools-generic linux-tools-$(uname -r)` | `perf` for `scripts/perf.sh stat|record` |

### Don't install / not needed
- **Don't install `linux-tools-$(uname -r)` on WSL2.** No package exists for the Microsoft kernel, so `apt` fails the whole command. Ubuntu's `perf` wrapper may still exist, but prints `perf not found for kernel …-microsoft`.
- **No test/benchmark frameworks.** The tests use the zero-dependency harness in `tests/testing.hpp`. Google Test, Catch2 and Google Benchmark are not used.
- `ninja` is optional. The presets don't select it, and builds use Makefiles.
- `clang-tidy` is optional for now. It's planned for CI in Phase 7 but not wired up yet.
- No Python, Conan/vcpkg, or Boost. The project has no third-party dependencies.

### Install commands
**Ubuntu / Debian, native Linux:**
```bash
sudo apt update
sudo apt install -y build-essential g++ clang cmake git valgrind gdb clang-format \
                    linux-tools-common linux-tools-generic linux-tools-$(uname -r)
```
**Ubuntu on WSL2** (the same list without `linux-tools-*`):
```bash
sudo apt update
sudo apt install -y build-essential g++ clang cmake git valgrind gdb clang-format
```
**Fedora:** `sudo dnf install -y gcc-c++ clang cmake git perf valgrind gdb clang-tools-extra`
**Arch:** `sudo pacman -S base-devel clang cmake git perf valgrind gdb`

If your distro's CMake is older than 3.20: `pip install cmake --break-system-packages` or
`sudo snap install cmake --classic`.

**Verify the toolchain:**
```bash
g++ --version        # >= 11
cmake --version      # >= 3.20
clang++ --version    # optional, >= 14
valgrind --version   # optional
perf --version       # native Linux only; on WSL2 expect "perf not found for kernel"
```

## 3. Where to put the repository
- Keep the repo **inside the Linux filesystem** (e.g. `~/projects/lob-engine`).
- On WSL2, don't build under `/mnt/c/...`: I/O is much slower, and `make` reports `Clock skew detected`.
- To open the repo from Windows, use `\\wsl.localhost\Ubuntu-24.04\home\<user>\projects\lob-engine`.
- The Windows-side PowerShell has no `git`. Run git and build commands inside WSL: `wsl -d Ubuntu-24.04 -- bash -lc "cd ~/projects/lob-engine && …"`.
- Moving to another machine: there is no git remote yet. Use `git bundle create lob.bundle --all`, then `git clone lob.bundle lob-engine`, or add a private remote.
- `data/*.bin` is git-ignored, so regenerate it on the new machine (§6). Its checksums prove it matches (§8).

## 4. Build

| Preset | Build type / flags | Use it for |
|---|---|---|
| `release` | `RelWithDebInfo` = `-O2 -g -DNDEBUG`, **`-march=native`**, frame pointers | benchmarks, perf work (binary only runs on CPUs like yours) |
| `debug` | `-O0 -g` | fast iteration, gdb |
| `asan` | Debug + `-fsanitize=address,undefined -fno-sanitize-recover=all` | pre-commit memory/UB gate |
| `tsan` | Debug + `-fsanitize=thread` | Phase 6 concurrency; runtime fails on WSL2 |

All presets add `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-omit-frame-pointer`.
```bash
scripts/build.sh debug                     # or release | asan | tsan  (default: release)
ctest --test-dir build/debug --output-on-failure
cmake --preset asan -DCMAKE_CXX_COMPILER=clang++   # reproduce a clang CI job (fresh build dir)
```
**Editor:** CMake writes `build/<preset>/compile_commands.json`. For clangd, symlink it to the repo root with `ln -sf build/debug/compile_commands.json .`.

## 5. Verify the installation (first-run checklist)

Run these in order; each has an expected result.

| # | Command | Expect |
|---|---|---|
| 1 | `scripts/build.sh debug && build/debug/lob_tests \| tail -1` | `46 tests, 0 failed checks` (count grows over time; failures must be 0). No compiler warnings. |
| 2 | `scripts/check.sh` | ends with `CHECK PASSED` (ASan is ~10× slower, ~35 s of tests) |
| 3 | `scripts/build.sh release && build/release/gen_events data/events_1M.bin 1000000 42` | `wrote 1000000 events (32.0 MB)` |
| 4 | `build/release/replay_cli data/events_1M.bin` | `trades: 528626 (volume 13489553)`, `cancel misses: 120860`, `resting at end: 185922`, `book checksum : ba7911b3a1dd1d8a` |
| 5 | `build/release/bench_match 1000000` | header line with `checksum=ba7911b3a1dd1d8a`, 7 variant lines, no `checksum mismatch`; exit code 0 |
| 6 | `build/release/bench_match 1000 \| grep tsc` (x86) | `invariant=yes` on modern CPUs; ns/tick ≈ 1 / your base clock |
| 7 | `cat /sys/devices/system/clocksource/clocksource0/current_clocksource` | usually `tsc`; record it with latency numbers |
| 8 | native only: `perf stat -e cycles,instructions true` | real counts, not `<not supported>` (see §7 if permission denied) |

If step 4's checksum differs, the build or the generated file is wrong. Don't benchmark until it matches.

## 6. Tool reference

### `gen_events <out.bin> [count=5000000] [seed=42]`
Writes a deterministic synthetic feed (32-byte events: ~70% limit, ~10% market, ~20% cancel).
The same count and seed always produce byte-identical output.
```bash
build/release/gen_events data/events_1M.bin 1000000 42
build/release/gen_events data/events_5000000.bin 5000000 42   # name used by scripts/bench.sh and perf.sh
```

### `replay_cli <events.bin> [options]`
Replays a log through the book and prints the load time, events, trades, rejects, cancel misses, throughput, VWAP, checksum.
The log is memory-mapped by default (zero-copy, validated). A malformed file is rejected with a one-line reason.

| Option | Effect |
|---|---|
| `--read` | copy the log into a vector (the pre-`mmap` reader) instead of mapping it; `native_run.sh` uses it for `perf stat` so counters stay comparable across days |
| `--latency` | per-event latency percentiles (p50/p90/p99/p99.9) from a log-bucket histogram, plus the timer's own overhead (included in every sample) |
| `--tsc` | with `--latency`: use the calibrated `rdtscp` timer instead of `steady_clock` |
| `--no-strategy` | disable the VWAP strategy hook (required for storage variants below) |
| `--naive` | per-order `new/delete` storage (Phase 2a baseline) |
| `--order-size 48\|64` | padded order records (Phase 2b) |
| `--hot-cold` | hot/cold split storage (Phase 2c) |
| `--tree` | `std::map` level store. Dense levels (a re-centring bitmap band) are the default since 2026-09-28; `--dense` is no longer accepted |
| `--sorted` | sorted-vector level store (Phase 3c) |
| `--huge-pages` | default book with the pool and id index on 2 MiB-aligned `MADV_HUGEPAGE` storage (Phase 2d); prints the process's `AnonHugePages` at the end of replay. Takes effect only when THP is `madvise` or `always` |

Storage variants are mutually exclusive and need `--no-strategy` (`--naive` excepted). Each one
exists so that `perf stat` can measure a single variant in isolation.

### `bench_match [events=2000000] [active-levels=0] [market-trade-percent=0]`
Runs 8 storage/level variants (the 7 above plus `32B huge`), 7 alternating runs each, and prints median (min–max) events/s,
then latency percentiles for both timers. Event generation and the book-shape pass run outside
timed regions. `active-levels > 0` switches to the controlled-depth generator; `market-trade-percent`
(0–100) needs `active-levels > 0`. It exits with code 1 if any variant's checksum differs.
```bash
build/release/bench_match 1000000            # standard stream
build/release/bench_match 1000000 25 25      # exactly 25 levels, 25% configured market flow
```

### Scripts
| Script | What it does |
|---|---|
| `scripts/build.sh [preset]` | configure + build (default `release`) |
| `scripts/check.sh` | ASan/UBSan build + tests, then release build + tests. **Run before every commit.** |
| `scripts/bench.sh [N=5000000] [core=2]` | release build, generates `data/events_N.bin` if missing, pinned `replay_cli --latency` + `bench_match N` |
| `scripts/perf.sh stat [file]` | `perf stat`: cycles, instructions, branches/misses, cache refs/misses, L1d misses (native only) |
| `scripts/perf.sh record [file]` | sampling profile with frame-pointer call graphs → `perf report` (native only) |
| `scripts/perf.sh cache [file]` | Cachegrind on a separate non-native `build/cache` build (works everywhere, slow) |
| `scripts/native_setup.sh` | native/live-USB session: installs toolchain + perf, sets perf permissions and governor/EPP (needs sudo) |
| `scripts/native_run.sh [full\|variance] [dir]` | the whole native measurement set with automatic logging and `summary.md`; see `NATIVE-LIVE-USB.md` |
| `scripts/native_compare.sh [dir]` | cross-run/cross-day spread of `bench_match` medians (Phase 4 < 5% check) |

`perf.sh` defaults to `data/events_5000000.bin`. For per-function instruction counts:
`valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=cg.out build/cache/replay_cli <file> --no-strategy && cg_annotate cg.out`.

## 7. What to ensure before trusting a number

### Machine setup
**`perf` permissions (native):**
```bash
cat /proc/sys/kernel/perf_event_paranoid                       # > 1 blocks most counters
sudo sysctl -w kernel.perf_event_paranoid=1                     # until reboot
echo 'kernel.perf_event_paranoid=1' | sudo tee /etc/sysctl.d/99-perf.conf   # persist
```
In a VM, `<not supported>` events are a platform limit, not a project bug. Use `scripts/perf.sh cache` instead.

**CPU governor and boost (native):**
```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
cat /sys/devices/system/cpu/cpufreq/boost        # AMD: 1 = boost on; record it (or disable for stability)
```
WSL2 exposes no governor, so record "not exposed".

### Per-run checklist
- [ ] `release` preset (note that it uses `-march=native`), pinned with `taskset -c <core>` (not core 0; avoid the SMT sibling of the core you use)
- [ ] On AC power; browsers, IDE indexers and other heavy processes closed
- [ ] ≥ 7 runs; report median **and** min–max, never only the best
- [ ] Latency reported as percentiles plus the timer overhead printed next to them; never the mean alone
- [ ] Checksum matches §8 for the workload, so you know you measured the same work
- [ ] Recorded: CPU, cores/SMT, RAM, kernel, compiler + version, flags, commit, event count, seed, clocksource, governor/boost
- [ ] Row appended to `docs/03-benchmark-log.md` (never edit old rows)

Expect large variance on WSL2 and laptops. On WSL2 the same 1M benchmark has differed by ~40%
between sessions, while spread within one session was 1–5%. Compare A vs B within one
invocation, and don't publish absolute latencies from a VM as bare-metal figures.

### Before every commit (definition of done, from `02-workflow.md`)
- [ ] `scripts/check.sh` → `CHECK PASSED`, with no new compiler warnings
- [ ] Differential test passes; replay checksums unchanged (§8)
- [ ] For performance changes: benchmark row, a counter that explains the delta (or Cachegrind on WSL2), and a finished experiment note
- [ ] After touching the replay loop, check that the **unmeasured** path's instruction count didn't change. Measurement code once pushed map/index calls out of line (+27 instructions/event). See `learning/experiments/2026-09-27-phase4-tsc-histogram.md`.

## 8. Golden checksums (seed 42)

All storage variants and level stores must produce the same checksum for a workload.

| Workload | Command | Checksum | Trades |
|---|---|---|---|
| 1M standard | `replay_cli data/events_1M.bin` / `bench_match 1000000` | `ba7911b3a1dd1d8a` | 528,626 |
| 5M standard | `replay_cli data/events_5000000.bin` / `bench_match 5000000` | `01e9c2940a0e996f` | 3,406,476 |
| 1M, 10 levels, no trades | `bench_match 1000000 10` | `696de5d2d747bf6f` | 0 |
| 1M, 25 levels, no trades | `bench_match 1000000 25` | `81dfcc26659fbde6` | 0 |
| 1M, 50 levels, no trades | `bench_match 1000000 50` | `30c25a6d88c9bb68` | 0 |
| 1M, 10 levels, 25% | `bench_match 1000000 10 25` | `47437d60709689bf` | 200,040 |
| 1M, 25 levels, 25% | `bench_match 1000000 25 25` | `554d243c4f1c0a38` | 200,036 |
| 1M, 50 levels, 25% | `bench_match 1000000 50 25` | `af1805de62b00573` | 200,026 |

A changed checksum means behavior changed. That's a bug unless the change was intended and
documented, for example a new event type. For reference throughput and latency on the same CPU
under WSL2, see "WSL2 reference data" in `last-task-completed.md`.

## 9. Daily workflow: where to start

1. Open `docs/last-task-completed.md` (current checkpoint and next task) and the matching phase in `docs/00-roadmap.md`.
2. Follow the 8-step loop in `docs/02-workflow.md` for every change: hypothesis → test first → implement → check → measure → explain → record → commit.
3. Each session, copy `docs/learning/notes/TEMPLATE.md` to `docs/learning/notes/$(date +%F)-phaseN.md` and fill it in.
4. For each experiment, copy `docs/learning/experiments/TEMPLATE.md` and write the hypothesis *before* the code.
5. Re-implement, don't copy. For each phase, write the code from scratch first, then compare with the reference.
6. Keep commits small and local. Branch as `phaseN/<topic>`; don't push unless you mean to.

## 10. Common problems

| Symptom | Likely cause / fix |
|---|---|
| `apt` fails: `Unable to locate package linux-tools-6.x-microsoft…` | You're on WSL2. Drop `linux-tools-*` from the install (§2); `perf` isn't available there. |
| `perf not found for kernel …` / `<not supported>` events | WSL2 or VM without PMU passthrough. Use `scripts/perf.sh cache`, and run counters on native Linux. |
| `perf` permission denied | `perf_event_paranoid` too high (§7) |
| TSan binary aborts at startup on WSL2 | Unsupported runtime there. The build succeeding is fine; run TSan on native Linux. |
| `make: Clock skew detected` / very slow builds | Repo is under `/mnt/c` on WSL2. Move it into `~/`. |
| `git` not found in Windows PowerShell | Run git inside WSL (§3) |
| `cmake: command not found` / too old | install per §2 |
| ASan run is very slow | Expected (~5–10×). Use `debug` for iteration and `asan` for the pre-commit gate. |
| Benchmark numbers jump > 20% between runs or sessions | Frequency scaling, thermal limits, other processes, or the WSL2 host. Fix §7, increase runs, and compare within one invocation. |
| `bench_match` prints `checksum mismatch` / exit 1 | A variant diverged in behavior. Treat it as a correctness bug (`02-workflow.md` → debugging checklist). |
| `replay_cli` checksum ≠ §8 | Wrong or regenerated file (check count and seed) or a behavior change. Don't benchmark until resolved. |
| `storage variants are exclusive and require --no-strategy` | Add `--no-strategy`; use one storage flag at a time |
| `-march=native` binary crashes elsewhere (`Illegal instruction`) | Release preset is CPU-specific. Rebuild on the target machine. |
| `rdtscp` latency not lower than `steady_clock` | Expected when the clocksource is `tsc` (both ~20 ns). Its `lfence` also costs throughput. |
| Differential test fails after your change | `02-workflow.md` → "Debugging checklist when a differential test fails" |

## 11. What "good" looks like before moving to the next phase

- `scripts/check.sh` is green, and CI (g++ and clang++ × debug/asan/release) would be too.
- You can explain every invariant in `docs/01-architecture.md §4`, and why the code upholds it.
- Every number you'd quote has a benchmark-log row with hardware, compiler, flags, commit and run count.
- At least one filled-in note in `docs/learning/notes/`, and one experiment in `docs/learning/experiments/` per optimization.
- You never skipped writing the test before the implementation.
