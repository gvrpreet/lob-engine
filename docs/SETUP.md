# Setup & Getting Started (Linux)

This is the on-ramp. Read this once, run the commands, then move to `docs/00-roadmap.md`
for the actual learning/build plan and `docs/02-workflow.md` for the day-to-day loop.

---
## 1. Install the toolchain

You need: a C++20 compiler, CMake ≥ 3.20, and (optional but strongly recommended) sanitizers,
`perf`, and `valgrind` for the measurement phases.

**Debian/Ubuntu:**
```bash
sudo apt update
sudo apt install -y build-essential g++ clang cmake ninja-build git \
                     linux-tools-common linux-tools-generic linux-tools-$(uname -r) \
                     valgrind clang-tidy clang-format gdb
```
- `linux-tools-*` gives you `perf`. If `perf` still fails with a permissions error, see §5.
- If your distro's CMake is older than 3.20 (`cmake --version`), install a newer one instead:
  ```bash
  pip install cmake --break-system-packages   # or: sudo snap install cmake --classic
  ```

**Fedora:**
```bash
sudo dnf install -y gcc-c++ clang cmake ninja-build git perf valgrind clang-tools-extra gdb
```

**Arch:**
```bash
sudo pacman -S base-devel clang cmake ninja git perf valgrind gdb
```

Verify:
```bash
g++ --version        # need >= GCC 11 (13 is what this was built/tested with)
cmake --version       # need >= 3.20
```

## 2. Get the project building

```bash
cd lob-engine
scripts/build.sh debug          # fastest to compile, best for iterating + gdb
ctest --test-dir build/debug --output-on-failure
```
If that passes, you're set up correctly. Now run the full gate once so you know it works:
```bash
scripts/check.sh                # ASan+UBSan build + release build, both tested
```
This takes longer (ASan instrumentation is slow) but is the thing you run before every commit.

## 3. Editor setup (recommended: VS Code or any clangd-based editor)

CMake is configured to emit `compile_commands.json` automatically. Point your editor's clangd
extension at `build/debug/compile_commands.json` (or symlink it to the repo root):
```bash
ln -sf build/debug/compile_commands.json .
```
This gets you accurate autocomplete, go-to-definition, and inline diagnostics matching the
actual flags (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`) used to build.

## 4. Generate data and run the pieces once, by hand

Before automating anything, run each tool manually so you understand what it does:
```bash
scripts/build.sh release
build/release/gen_events   data/events_1M.bin 1000000 42     # deterministic synthetic feed
build/release/replay_cli   data/events_1M.bin --latency      # run it through the book
build/release/bench_match  1000000                            # throughput/latency microbench
build/release/lob_tests                                       # or: ctest --test-dir build/release
```
Read `tools/replay_cli.cpp` and `bench/bench_match.cpp` — they're short and are the whole
"how do I actually exercise this engine" story.

## 5. `perf` permissions (needed for `scripts/perf.sh stat` / `record`)

Most distros lock down `perf` by default. Either run with `sudo`, or relax it for your session:
```bash
# check current value
cat /proc/sys/kernel/perf_event_paranoid
# temporarily allow (resets on reboot)
sudo sysctl -w kernel.perf_event_paranoid=1
# persist it
echo 'kernel.perf_event_paranoid=1' | sudo tee /etc/sysctl.d/99-perf.conf
```
If you're in a VM/container without access to hardware counters, `perf stat` may report
`<not supported>` for some events — that's a platform limit, not a bug in the project.
`valgrind --tool=cachegrind` (used by `scripts/perf.sh cache`) works everywhere since it
doesn't need hardware counters, just slower.

## 6. Get consistent benchmark numbers (do this before trusting any number)

Noise ruins measurement, and measurement is half the point of this project. On a real (non-VM,
non-shared) Linux machine:
```bash
# see available governors
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
# pin to 'performance' (undo by setting back to 'powersave'/'ondemand' or reboot)
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```
Then always run benchmarks pinned to a specific core, away from CPU 0 (which handles most
interrupts):
```bash
scripts/bench.sh 5000000 2       # 5M events, pinned to core 2
```
If you're on a laptop, plug in AC power (battery mode throttles), close browsers/Slack, and
expect more run-to-run variance than a dedicated box — just be honest about that in
`docs/03-benchmark-log.md`.

If you're in a cloud VM or container (no governor control, shared physical cores), you can
still learn the relative effect of a change (before vs after, same machine, same run), just
don't publish absolute latency numbers as if they were bare-metal figures.

## 7. Daily workflow — where to actually start

1. Open `docs/00-roadmap.md` and `docs/last-task-completed.md`. The latter is the current checkpoint and names the next build task. Check the phase's status and remaining criteria before starting.
2. Open `docs/02-workflow.md` and follow the 8-step loop for every change you make.
3. Copy `docs/learning/notes/TEMPLATE.md` to `docs/learning/notes/$(date +%F)-phaseN.md` each
   session and fill it in — this is what turns "I fiddled with code" into a portfolio.
4. First real exercise (Phase 1): delete your mental model, then re-derive `include/lob/order_book.hpp`
   from scratch in a scratch file, using only `docs/01-architecture.md` as reference. Diff against
   the real one. Where you differ, that's the thing to understand, not just copy.
5. Before every commit: `scripts/check.sh`. Before claiming a performance win: `scripts/bench.sh`
   plus a `perf stat` counter that explains *why* (see `docs/02-workflow.md` §Profiling recipes).

## 8. Common problems

| Symptom | Likely cause / fix |
|---|---|
| `cmake: command not found` | install per §1, or `pip install cmake --break-system-packages` |
| ASan build is very slow / test times out | expected — ASan instrumentation ~5-10x slower; use `debug` preset for quick iteration, `asan` only for the pre-commit gate |
| `perf stat` shows `<not supported>` events | running in a VM/container without HW counter passthrough; use `valgrind --tool=cachegrind` instead |
| Benchmark numbers jump around >20% between runs | CPU frequency scaling / thermal throttling / other processes; fix governor (§6), close other programs, increase run count |
| `-march=native` build fails to run elsewhere | that flag bakes in your exact CPU's instruction set; only use it for local perf experiments, never for anything you'd hand to someone else |
| Differential test fails after your change | see `docs/02-workflow.md` → "Debugging checklist when a differential test fails" |

## 9. What "good" looks like before you move to the next phase

- `scripts/check.sh` is green.
- You can explain, out loud, every invariant in `docs/01-architecture.md §4` and why the code
  upholds it.
- You have at least one filled-in note in `docs/learning/notes/` and, once you start optimizing
  in Phase 2+, at least one filled-in experiment in `docs/learning/experiments/`.
- You haven't skipped writing the test before the implementation, even once.
