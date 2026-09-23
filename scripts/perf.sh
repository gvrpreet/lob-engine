#!/usr/bin/env bash
# usage: scripts/perf.sh stat|record|cache [events.bin]
# stat   : IPC, branch-misses, cache-misses      record : sampling profile (perf report)
# cache  : valgrind cachegrind (slow, deterministic; great for reasoning about layout)
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:?stat|record|cache}"; F="${2:-data/events_5000000.bin}"; BIN=build/release/replay_cli
case "$MODE" in
  stat)   perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-load-misses \
            taskset -c 2 $BIN "$F" ;;
  record) perf record -g --call-graph fp -o data/perf.data taskset -c 2 $BIN "$F" && perf report -i data/perf.data ;;
  cache)  valgrind --tool=cachegrind --cache-sim=yes $BIN "$F" ;;
esac
