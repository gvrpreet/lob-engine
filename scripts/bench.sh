#!/usr/bin/env bash
# usage: scripts/bench.sh [N events] [cpu core]   -- pinned, reproducible benchmark run
set -euo pipefail
cd "$(dirname "$0")/.."
N="${1:-5000000}"; CORE="${2:-2}"
scripts/build.sh release >/dev/null
BIN=build/release
[ -f "data/events_${N}.bin" ] || $BIN/gen_events "data/events_${N}.bin" "$N" 42
echo "# $(git rev-parse --short HEAD 2>/dev/null || echo nogit) | $(lscpu | grep 'Model name' | sed 's/.*: *//') | $(c++ --version | head -1)"
taskset -c "$CORE" $BIN/replay_cli "data/events_${N}.bin" --latency
taskset -c "$CORE" $BIN/bench_match "$N"
