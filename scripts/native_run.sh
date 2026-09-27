#!/usr/bin/env bash
# Native-host measurement run with automatic logging. Manual: docs/NATIVE-LIVE-USB.md
# usage: scripts/native_run.sh [full|variance] [results-root=results]
#   full      day 1 (~25 min): environment, check.sh, TSan, golden checksums, perf stat per
#             storage variant, THP never vs always, both latency timers, every bench_match
#             workload, scripts/bench.sh
#   variance  days 2 and 3 (~8 min): environment, golden checksums, scripts/bench.sh and two
#             bench_match workloads -- the Phase 4 cross-day variance check
# env: CORE=2 (pinned core), QUICK=1 (small event counts, skips check/TSan: smoke test only)
# Output: <root>/<date>_<time>_<mode>/ holding transcript.log (everything printed), NN-<step>.log
# per step, env.txt, steps.tsv, golden.tsv, results.tsv, latency.tsv, perf.tsv and summary.md.
set -uo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-full}"
ROOT="${2:-results}"
CORE="${CORE:-2}"
QUICK="${QUICK:-0}"
case "$MODE" in full | variance) ;; *) echo "usage: $0 [full|variance] [results-root]" >&2; exit 2 ;; esac

STAMP="$(date +%Y-%m-%d_%H%M%S)"
OUT="$ROOT/${STAMP}_${MODE}"
[ "$QUICK" = 1 ] && OUT="${OUT}_quick"
mkdir -p "$OUT" || { echo "cannot create $OUT" >&2; exit 1; }
OUT="$(cd "$OUT" && pwd)"
BIN=build/release
if [ "$QUICK" = 1 ]; then EV1=100000; EV5=200000; else EV1=1000000; EV5=5000000; fi

declare -A GOLD=(
  ["1000000"]=ba7911b3a1dd1d8a ["5000000"]=01e9c2940a0e996f
  ["1000000 10"]=696de5d2d747bf6f ["1000000 25"]=81dfcc26659fbde6 ["1000000 50"]=30c25a6d88c9bb68
  ["1000000 10 25"]=47437d60709689bf ["1000000 25 25"]=554d243c4f1c0a38 ["1000000 50 25"]=af1805de62b00573
)
PERF_EVENTS=cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,dTLB-loads,dTLB-load-misses,page-faults
STEP=0
LAST_LOG=
HAVE_PERF=0

sysval() { cat "$1" 2>/dev/null || echo n/a; }
counts() { cat "$@" 2>/dev/null | sort | uniq -c | xargs; }   # "16 performance" style summary

step() {  # step <name> <command...>: run it, save output to NN-<name>.log, record exit code + seconds
  local name=$1; shift
  STEP=$((STEP + 1))
  LAST_LOG="$(printf '%s/%02d-%s.log' "$OUT" "$STEP" "$name")"
  local t0=$SECONDS
  printf '\n===== [%02d] %s: %s\n' "$STEP" "$name" "$*"
  "$@" 2>&1 | tee "$LAST_LOG"
  local rc=${PIPESTATUS[0]}
  printf '%02d\t%s\t%s\t%s\n' "$STEP" "$name" "$rc" "$((SECONDS - t0))" >> "$OUT/steps.tsv"
  printf -- '----- [%02d] %s exit=%s (%ss)\n' "$STEP" "$name" "$rc" "$((SECONDS - t0))"
}

collect_env() {
  local cpu=/sys/devices/system/cpu
  echo "date: $(date -Is)"
  echo "mode: $MODE quick=$QUICK core=$CORE events=$EV1/$EV5"
  echo "commit: $(git rev-parse --short HEAD 2>/dev/null || echo nogit) dirty=$(git status --porcelain 2>/dev/null | grep -q . && echo yes || echo no)"
  echo "host: $(hostname)"
  echo "kernel: $(uname -r)"
  echo "os: $(. /etc/os-release && echo "$PRETTY_NAME")"
  echo "live_session: $(grep -q 'boot=casper' /proc/cmdline && echo yes || echo no)"
  echo "wsl: $(grep -qi microsoft /proc/sys/kernel/osrelease && echo yes || echo no)"
  echo "cpu: $(lscpu | sed -n 's/^Model name: *//p')"
  echo "threads: $(nproc) per_core=$(lscpu | sed -n 's/^Thread(s) per core: *//p') core${CORE}_siblings=$(sysval $cpu/cpu$CORE/topology/thread_siblings_list)"
  echo "mem: $(free -h | awk '/Mem:/{print $2 " total, " $7 " available"}')"
  echo "scaling_driver: $(sysval $cpu/cpu$CORE/cpufreq/scaling_driver)"
  echo "governor: $(counts $cpu/cpu*/cpufreq/scaling_governor)"
  echo "epp: $(counts $cpu/cpu*/cpufreq/energy_performance_preference)"
  echo "boost: global=$(sysval $cpu/cpufreq/boost) core$CORE=$(sysval $cpu/cpu$CORE/cpufreq/boost)"
  echo "freq_khz_core$CORE: cur=$(sysval $cpu/cpu$CORE/cpufreq/scaling_cur_freq) min=$(sysval $cpu/cpu$CORE/cpufreq/scaling_min_freq) max=$(sysval $cpu/cpu$CORE/cpufreq/scaling_max_freq)"
  echo "power_profile: $(powerprofilesctl get 2>/dev/null || echo n/a)"
  echo "ac_online: $(cat /sys/class/power_supply/A*/online 2>/dev/null | head -1 | grep . || echo n/a)"
  echo "battery_pct: $(cat /sys/class/power_supply/BAT*/capacity 2>/dev/null | head -1 | grep . || echo n/a)"
  echo "clocksource: $(sysval /sys/devices/system/clocksource/clocksource0/current_clocksource) (available: $(sysval /sys/devices/system/clocksource/clocksource0/available_clocksource))"
  echo "thp: enabled=$(sysval /sys/kernel/mm/transparent_hugepage/enabled) defrag=$(sysval /sys/kernel/mm/transparent_hugepage/defrag)"
  echo "perf_event_paranoid: $(sysval /proc/sys/kernel/perf_event_paranoid)"
  echo "loadavg: $(cat /proc/loadavg)"
  echo "temps_c: $(for z in /sys/class/thermal/thermal_zone*; do [ -e "$z/temp" ] && printf '%s=%s ' "$(cat "$z/type")" "$(($(cat "$z/temp") / 1000))"; done)"
  echo "gcc: $(g++ --version | head -1)"
  echo "clang: $(clang++ --version 2>/dev/null | head -1 || echo n/a)"
  echo "cmake: $(cmake --version | head -1)"
  echo "perf: $(perf --version 2>&1 | head -1)"
  echo "release_flags: $(sed -n 's/^CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=//p' $BIN/CMakeCache.txt 2>/dev/null) native=$(sed -n 's/^LOB_NATIVE:BOOL=//p' $BIN/CMakeCache.txt 2>/dev/null)"
  echo
  echo "top processes by CPU:"
  ps -eo pcpu,pid,comm --sort=-pcpu | head -8
  echo
  lscpu
}

preflight() {  # warnings do not stop the run; they are recorded in warnings.txt and summary.md
  local env=$OUT/env.txt
  {
    grep -q '^wsl: yes' "$env" && echo "WSL2 detected: this is not a native run"
    grep -q '^live_session: yes' "$env" || echo "not a live-USB session (fine if this is an installed native system)"
    grep '^governor:' "$env" | grep -qv 'performance' && echo "governor is not 'performance' on all CPUs: run scripts/native_setup.sh"
    grep -q '^ac_online: 0' "$env" && echo "running on battery: plug in AC"
    grep -q 'dirty=yes' "$env" && echo "git working tree is dirty: results are not tied to a clean commit"
    [ "$HAVE_PERF" = 1 ] || echo "perf counters unavailable: perf and THP steps skipped"
    awk '{ if ($1 > 1.0) print "1-minute load average " $1 " > 1.0: close other programs" }' /proc/loadavg
    [ "$QUICK" = 1 ] && echo "QUICK=1: smoke-test event counts; numbers are not comparable"
  } | tee "$OUT/warnings.txt" | sed 's/^/WARN: /'
}

gen_data() {
  local n
  for n in "$EV1" "$EV5"; do
    if [ -f "data/events_$n.bin" ]; then echo "data/events_$n.bin exists"; else $BIN/gen_events "data/events_$n.bin" "$n" 42; fi
  done
}

golden_replay() {  # golden_replay <events>: replay the file and compare with the known checksum
  local n=$1 want got status
  want=${GOLD[$n]:-n/a}
  got=$(taskset -c "$CORE" $BIN/replay_cli "data/events_$n.bin" --no-strategy | tee /dev/stderr | awk '/book checksum/{print $4}')
  if [ "$want" = n/a ]; then status=unknown; elif [ "$got" = "$want" ]; then status=OK; else status=MISMATCH; fi
  printf 'replay_cli %s\t%s\t%s\t%s\n' "$n" "$want" "$got" "$status" >> "$OUT/golden.tsv"
  echo "golden: replay_cli $n want=$want got=$got $status"
  [ "$status" != MISMATCH ]
}

record_bench() {  # record_bench <workload> <log>: checksum -> golden.tsv, rates -> results.tsv, latency -> latency.tsv
  local w=$1 log=$2 want got status
  want=${GOLD[$w]:-n/a}
  got=$(sed -n 's/.*checksum=\([0-9a-f]*\).*/\1/p' "$log" | head -1)
  if [ "$want" = n/a ]; then status=unknown; elif [ "$got" = "$want" ]; then status=OK; else status=MISMATCH; fi
  printf 'bench_match %s\t%s\t%s\t%s\n' "$w" "$want" "$got" "$status" >> "$OUT/golden.tsv"
  awk -v s="$STAMP" -v m="$MODE" -v w="$w" '/ median=/ {
      line = $0; label = line; sub(/ +median=.*/, "", label)
      med = line; sub(/.*median=/, "", med); sub(/M.*/, "", med)
      mn = line;  sub(/.*min=/, "", mn);     sub(/M.*/, "", mn)
      mx = line;  sub(/.*max=/, "", mx);     sub(/M.*/, "", mx)
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", s, m, w, label, med, mn, mx
    }' "$log" >> "$OUT/results.tsv"
  awk -v s="$STAMP" -v w="$w" '/^latency ns / {
      ov = $6; sub(/.*=/, "", ov); sub(/,/, "", ov)
      p50 = $8; p90 = $9; p99 = $10; p999 = $11
      sub(/.*=/, "", p50); sub(/.*=/, "", p90); sub(/.*=/, "", p99); sub(/.*=/, "", p999)
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", s, w, $3, ov, p50, p90, p99, p999
    }' "$log" >> "$OUT/latency.tsv"
}

bench() {  # bench <bench_match args...>
  local args="$*"
  step "bench_match_${args// /_}" taskset -c "$CORE" $BIN/bench_match "$@"
  record_bench "$*" "$LAST_LOG"
}

perf_variant() {  # perf_variant <events> <label> [replay_cli flags...]
  local n=$1 label=$2; shift 2
  step "perf_${n}_${label}" taskset -c "$CORE" perf stat -r 5 -e "$PERF_EVENTS" -- \
    $BIN/replay_cli "data/events_$n.bin" --no-strategy "$@"
  awk -v w="$n" -v v="$label" -v evs="$PERF_EVENTS" '
    BEGIN { k = split(evs, a, ","); for (i = 1; i <= k; i++) want[a[i]] = 1 }
    {
      if ($1 == "<not") {  # "<not supported>  <event>" or "<not counted>  <event>"
        name = $3; sub(/:.*/, "", name)
        if (name in want) printf "%s\t%s\t%s\tn/a\t%s\n", w, v, name, $2
        next
      }
      name = $2; sub(/:.*/, "", name)
      if (!(name in want) || $1 !~ /^[0-9][0-9,.]*$/) next
      val = $1; gsub(/,/, "", val); var = ""
      if (match($0, /\+- +[0-9.]+%/)) { var = substr($0, RSTART, RLENGTH); gsub(/[+ -]/, "", var) }
      printf "%s\t%s\t%s\t%s\t%s\n", w, v, name, val, var
    }' "$LAST_LOG" >> "$OUT/perf.tsv"
}

thp_mode() {  # thp_mode <never|always>: switch THP system-wide, then count dTLB misses on the big log
  echo "$1" | sudo -n tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null || return 1
  echo "thp enabled: $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
  taskset -c "$CORE" perf stat -r 5 -e dTLB-loads,dTLB-load-misses,page-faults,cycles,instructions -- \
    $BIN/replay_cli "data/events_$EV5.bin" --no-strategy
}

md_table() {  # md_table <tsv> <header cells...>: print a TSV file as a Markdown table
  local file=$1; shift
  [ -s "$file" ] || { echo "_none_"; return; }
  local IFS='|'; echo "| $* |"
  printf '|'; for _ in "$@"; do printf -- '---|'; done; echo
  awk -F'\t' '{ out = "|"; for (i = 1; i <= NF; i++) out = out " " $i " |"; print out }' "$file"
}

write_summary() {
  local s=$OUT/summary.md
  {
    echo "# Native run summary: $STAMP ($MODE)"
    echo
    echo "Generated by \`scripts/native_run.sh\`. Raw output: \`transcript.log\` and the numbered step logs."
    echo
    echo "## Environment"
    echo '```'
    grep -E '^(date|mode|commit|kernel|os|live_session|wsl|cpu|threads|mem|scaling_driver|governor|epp|boost|freq_khz|power_profile|ac_online|clocksource|thp|perf_event_paranoid|loadavg|temps_c|gcc|perf|release_flags):' "$OUT/env.txt"
    echo '```'
    echo
    echo "## Warnings"
    if [ -s "$OUT/warnings.txt" ]; then sed 's/^/- /' "$OUT/warnings.txt"; else echo "none"; fi
    echo
    echo "## Steps"
    md_table "$OUT/steps.tsv" "#" step exit seconds
    echo
    echo "## Golden checksums"
    md_table "$OUT/golden.tsv" workload expected got status
    echo
    echo "## Throughput (M events/s, 7 runs per variant)"
    [ -s "$OUT/results.tsv" ] && cut -f3- "$OUT/results.tsv" > "$OUT/.results.md.tsv"
    md_table "$OUT/.results.md.tsv" workload variant median min max
    echo
    echo "## Latency (ns, timer overhead included)"
    [ -s "$OUT/latency.tsv" ] && cut -f2- "$OUT/latency.tsv" > "$OUT/.latency.md.tsv"
    md_table "$OUT/.latency.md.tsv" workload timer overhead_p50 p50 p90 p99 p99.9
    echo
    echo "## perf stat derived (replay_cli --no-strategy, 5 repeats; includes file load + book setup)"
    if [ -s "$OUT/perf.tsv" ]; then
      awk -F'\t' '
        function ok(x) { return x ~ /^[0-9]/ && x + 0 > 0 }
        function ratio(a, b, fmt) { return ok(a) && ok(b) ? sprintf(fmt, a / b) : "n/a" }
        { key = $1 "\t" $2; keys[key] = 1; v[key, $3] = $4 }
        END {
          for (k in keys) {
            split(k, p, "\t")
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", p[1], p[2],
              ratio(v[k, "instructions"], v[k, "cycles"], "%.2f"),
              ratio(v[k, "instructions"], p[1], "%.0f"),
              ratio(100 * v[k, "branch-misses"], v[k, "branches"], "%.2f%%"),
              ratio(v[k, "L1-dcache-load-misses"], p[1], "%.2f"),
              ratio(v[k, "cache-misses"], p[1], "%.2f"),
              ratio(v[k, "dTLB-load-misses"], p[1], "%.3f")
          }
        }' "$OUT/perf.tsv" | sort > "$OUT/.perf.md.tsv"
      md_table "$OUT/.perf.md.tsv" events variant IPC instr/event branch-miss L1d-miss/event cache-miss/event dTLB-miss/event
      echo
      echo "Raw counters with run-to-run variance: \`perf.tsv\`. THP comparison: see the \`thp_*\` step logs."
    else
      echo "_perf not available_"
    fi
    echo
    echo "## Next"
    echo "- Copy this whole folder back (see docs/NATIVE-LIVE-USB.md §7) and compare days with \`scripts/native_compare.sh <results-root>\`."
  } > "$s"
  rm -f "$OUT"/.*.md.tsv
  echo "summary written: $s"
}

main() {
  echo "lob-engine native run: mode=$MODE quick=$QUICK core=$CORE out=$OUT"
  if perf stat -e instructions -- true 2>&1 | grep -q ' instructions' &&
     ! perf stat -e instructions -- true 2>&1 | grep -q 'not supported'; then HAVE_PERF=1; fi

  step build_release scripts/build.sh release
  step env collect_env
  cp "$LAST_LOG" "$OUT/env.txt"
  preflight

  if [ "$MODE" = full ] && [ "$QUICK" != 1 ]; then
    step check scripts/check.sh
    step tsan bash -c 'scripts/build.sh tsan && ctest --test-dir build/tsan --output-on-failure'
  fi

  step gen_data gen_data
  step golden_${EV1} golden_replay "$EV1"
  step golden_${EV5} golden_replay "$EV5"

  if [ "$MODE" = full ]; then
    if [ "$HAVE_PERF" = 1 ]; then
      local n
      for n in "$EV1" "$EV5"; do
        perf_variant "$n" order32
        perf_variant "$n" naive --naive
        perf_variant "$n" order48 --order-size 48
        perf_variant "$n" order64 --order-size 64
        perf_variant "$n" hotcold --hot-cold
        perf_variant "$n" dense --dense
        perf_variant "$n" sorted --sorted
      done
      if sudo -n true 2>/dev/null; then
        local thp_orig
        thp_orig=$(sed 's/.*\[\(.*\)\].*/\1/' /sys/kernel/mm/transparent_hugepage/enabled)
        step thp_never thp_mode never
        step thp_always thp_mode always
        echo "$thp_orig" | sudo -n tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null
        echo "THP restored to $thp_orig"
      else
        echo "WARN: passwordless sudo unavailable; THP comparison skipped" | tee -a "$OUT/warnings.txt"
      fi
    fi
    step latency_steady taskset -c "$CORE" $BIN/replay_cli "data/events_$EV5.bin" --latency
    step latency_tsc taskset -c "$CORE" $BIN/replay_cli "data/events_$EV5.bin" --latency --tsc
    bench "$EV1"
    bench "$EV1" 10
    bench "$EV1" 25
    bench "$EV1" 50
    bench "$EV1" 10 25
    bench "$EV1" 25 25
    bench "$EV1" 50 25
  else
    bench "$EV1"
    bench "$EV1" 25 25
  fi

  step bench_sh scripts/bench.sh "$EV5" "$CORE"
  record_bench "$EV5" "$LAST_LOG"
  step env_end collect_env
  write_summary
  echo
  echo "DONE. Results in: $OUT"
  grep -q MISMATCH "$OUT/golden.tsv" 2>/dev/null && { echo "!!! CHECKSUM MISMATCH: see golden.tsv"; return 1; }
  awk -F'\t' '$3 != 0 { bad = 1 } END { exit !bad }' "$OUT/steps.tsv" && echo "note: some steps exited non-zero; see steps.tsv"
  return 0
}

main 2>&1 | tee "$OUT/transcript.log"
rc=${PIPESTATUS[0]}
sync
exit "$rc"
