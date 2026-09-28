#!/usr/bin/env bash
# usage: scripts/fuzz.sh [seconds=300] [jobs=1]
# Builds tests/fuzz_book.cpp with clang + libFuzzer + ASan/UBSan and fuzzes the default (dense)
# book and the tree book against the naive oracle. The corpus (build/fuzz-corpus) is kept, so
# later runs continue from it. Per-job logs, and any crash-* reproducer, go to build/fuzz-work.
# Ends with the total number of operations checked across all jobs.
set -euo pipefail
cd "$(dirname "$0")/.."
SECONDS_TO_RUN="${1:-300}"
JOBS="${2:-1}"
cmake -S . -B build/fuzz -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOB_FUZZ=ON > /dev/null
cmake --build build/fuzz --target fuzz_book -j"$(nproc)"
mkdir -p build/fuzz-corpus build/fuzz-work
cd build/fuzz-work
rm -f fuzz-*.log
status=0
../fuzz/fuzz_book ../fuzz-corpus -max_total_time="$SECONDS_TO_RUN" -jobs="$JOBS" -workers="$JOBS" \
  -print_final_stats=1 > fuzz-main.log 2>&1 || status=$?
# With -jobs, fuzz-main.log repeats the per-job logs: count the job logs only when they exist.
logs=$(ls fuzz-[0-9]*.log 2>/dev/null || echo fuzz-main.log)
total=$(cat $logs | awk '/fuzz_book: [0-9]+ operations checked/ {s += $2} END {print s + 0}')
grep -hE 'fuzz_book: [a-z]|ERROR|deadly|Done [0-9]+ runs' fuzz-*.log | grep -v 'operations checked' | sort | uniq -c || true
echo "operations checked: $total  (exit $status; corpus $(ls ../fuzz-corpus | wc -l) inputs)"
ls crash-* 2>/dev/null && echo "CRASH reproducers above (run: build/fuzz/fuzz_book <file>)"
exit $status
