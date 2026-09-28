#!/usr/bin/env bash
# Phase 5(b) measurement: throughput and peak memory (max RSS) per reader, on the 5M and 50M
# synthetic logs. Run from the repo root:  bash scripts/stream_measure.sh
# Output goes to the terminal and to results/stream_measure_<timestamp>.log.
# Needs a current Release build (scripts/build.sh release); takes about 5 minutes.
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p results
LOG="results/stream_measure_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee "$LOG") 2>&1
B=build/release
echo "commit $(git rev-parse --short HEAD) $(git status --short | grep -qv '^??' && echo '(dirty)')"
[ -f data/events_50000000.bin ] || $B/gen_events data/events_50000000.bin 50000000 42
for f in data/events_5000000.bin data/events_50000000.bin; do
  echo "================ $f"
  for mode in "" "--read" "--stream" "--stream --window 65536" "--stream --window 1024"; do
    runs=""
    for r in 1 2 3; do
      /usr/bin/time -f "%M" -o /tmp/sm_rss taskset -c 2 $B/replay_cli $f --no-strategy $mode > /tmp/sm_out
      runs="$runs $(awk '/throughput/{print $3}' /tmp/sm_out)M/$(( $(cat /tmp/sm_rss) / 1024 ))MB"
    done
    printf "%-24s ev/s / max RSS:%s | %s\n" "${mode:-mmap}" "$runs" "$(grep checksum /tmp/sm_out)"
  done
done
echo "DONE - log saved to $LOG"
