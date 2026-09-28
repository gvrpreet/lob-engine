#!/usr/bin/env bash
# Phase 5 exit: end-to-end replays of 5M+ events (synthetic and ITCH) with throughput, trades and
# strategy metrics, run twice; the two runs must match bit-for-bit apart from timing lines.
# Run from the repo root: bash scripts/phase5_exit.sh
set -uo pipefail
cd "$(dirname "$0")/.."
B=build/release
mkdir -p results
OUT=results/phase5_exit_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
echo "commit $(git rev-parse --short HEAD) $(git status --short | grep -qv '^??' && echo '(dirty)')" | tee "$OUT/commit.txt"
run_all() {  # $1 = run number
  local d="$OUT/run$1"; mkdir -p "$d"
  for f in data/events_5000000.bin data/events_50000000.bin data/itch/AAPL_20200130_c.bin \
           data/itch/AMD_20200130_c.bin data/itch/TQQQ_20200130_c.bin; do
    local n; n=$(basename "$f" .bin)
    taskset -c 2 $B/replay_cli "$f" > "$d/$n.mmap_vwap.txt"             # default reader + VWAP strategy
    taskset -c 2 $B/replay_cli "$f" --read > "$d/$n.read_vwap.txt"
    taskset -c 2 $B/replay_cli "$f" --stream > "$d/$n.stream_vwap.txt"
    taskset -c 2 $B/replay_cli "$f" --strategy mm --order-latency 1000 > "$d/$n.mm_1us.txt"
  done
}
run_all 1
run_all 2
echo "== bit-for-bit (timing lines removed)"
fails=0
for a in "$OUT"/run1/*.txt; do
  b="$OUT/run2/$(basename "$a")"
  if diff <(grep -vE '^(load|throughput)' "$a") <(grep -vE '^(load|throughput)' "$b") > /dev/null; then
    echo "  same  $(basename "$a")"
  else
    echo "  DIFF  $(basename "$a")"; fails=$((fails + 1))
  fi
done
echo "== readers agree (checksum, trades, vwap) per log"
for n in events_5000000 events_50000000 AAPL_20200130_c AMD_20200130_c TQQQ_20200130_c; do
  s=$(for m in mmap read stream; do grep -E 'checksum|trades|vwap' "$OUT/run1/$n.${m}_vwap.txt" | md5sum; done | sort -u | wc -l)
  echo "  $n: $([ "$s" = 1 ] && echo agree || echo DISAGREE)"
  [ "$s" = 1 ] || fails=$((fails + 1))
done
echo "== summary (run 1)"
for n in events_5000000 events_50000000 AAPL_20200130_c AMD_20200130_c TQQQ_20200130_c; do
  echo "--- $n"
  grep -E 'events|trades|throughput|vwap|checksum' "$OUT/run1/$n.mmap_vwap.txt" | sed 's/^/  /'
  grep -E 'mm fills|mm position|mm pnl|mm slippage|throughput' "$OUT/run1/$n.mm_1us.txt" | sed 's/^/  mm: /'
done
echo "RESULT: $fails mismatches. Output in $OUT"
